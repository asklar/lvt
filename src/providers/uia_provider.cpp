#include "uia_provider.h"
#include "../debug.h"

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <oleacc.h>
#include <UIAutomation.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lvt {
namespace {

using clock_type = std::chrono::steady_clock;

std::string narrow(BSTR bstr) {
    if (!bstr)
        return {};
    const int len = SysStringLen(bstr);
    if (len <= 0)
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, bstr, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, bstr, len, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

std::string trim_double(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}

// Render a VARIANT as the string lvt emits. UIA hands back a fairly narrow set
// of types, but the SAFEARRAY case matters: RuntimeId arrives as VT_ARRAY|VT_I4.
std::string variant_to_string(const VARIANT& v) {
    if (v.vt == VT_EMPTY || v.vt == VT_NULL)
        return {};

    if (v.vt == (VT_ARRAY | VT_I4)) {
        SAFEARRAY* sa = v.parray;
        if (!sa || SafeArrayGetDim(sa) != 1)
            return {};
        LONG lower = 0, upper = -1;
        if (FAILED(SafeArrayGetLBound(sa, 1, &lower)) || FAILED(SafeArrayGetUBound(sa, 1, &upper)))
            return {};
        std::string out;
        for (LONG i = lower; i <= upper; ++i) {
            int element = 0;
            if (FAILED(SafeArrayGetElement(sa, &i, &element)))
                return {};
            if (!out.empty())
                out += '.';
            out += std::to_string(element);
        }
        return out;
    }

    if (v.vt == (VT_ARRAY | VT_R8)) {
        SAFEARRAY* sa = v.parray;
        if (!sa || SafeArrayGetDim(sa) != 1)
            return {};
        LONG lower = 0, upper = -1;
        if (FAILED(SafeArrayGetLBound(sa, 1, &lower)) || FAILED(SafeArrayGetUBound(sa, 1, &upper)))
            return {};
        std::string out;
        for (LONG i = lower; i <= upper; ++i) {
            double element = 0;
            if (FAILED(SafeArrayGetElement(sa, &i, &element)))
                return {};
            if (!out.empty())
                out += ',';
            out += trim_double(element);
        }
        return out;
    }

    switch (v.vt) {
    case VT_BSTR: return narrow(v.bstrVal);
    case VT_BOOL: return v.boolVal != VARIANT_FALSE ? "true" : "false";
    case VT_I2:   return std::to_string(v.iVal);
    case VT_I4:   return std::to_string(v.lVal);
    case VT_INT:  return std::to_string(v.intVal);
    case VT_UI4:  return std::to_string(v.ulVal);
    case VT_R4:   return trim_double(v.fltVal);
    case VT_R8:   return trim_double(v.dblVal);
    default:      return {};
    }
}

// A few properties read better as names than as raw enum numbers.
std::string humanize(long propertyId, const VARIANT& v, const std::string& raw) {
    if (propertyId == UIA_ControlTypePropertyId && v.vt == VT_I4)
        return uia_control_type_name(v.lVal);

    if (propertyId == UIA_ToggleToggleStatePropertyId && v.vt == VT_I4) {
        switch (v.lVal) {
        case ToggleState_Off:           return "Off";
        case ToggleState_On:            return "On";
        case ToggleState_Indeterminate: return "Indeterminate";
        default: break;
        }
    }

    if (propertyId == UIA_ExpandCollapseExpandCollapseStatePropertyId && v.vt == VT_I4) {
        switch (v.lVal) {
        case ExpandCollapseState_Collapsed:    return "Collapsed";
        case ExpandCollapseState_Expanded:     return "Expanded";
        case ExpandCollapseState_PartiallyExpanded: return "PartiallyExpanded";
        case ExpandCollapseState_LeafNode:     return "LeafNode";
        default: break;
        }
    }

    if (propertyId == UIA_OrientationPropertyId && v.vt == VT_I4) {
        switch (v.lVal) {
        case OrientationType_None:       return "None";
        case OrientationType_Horizontal: return "Horizontal";
        case OrientationType_Vertical:   return "Vertical";
        default: break;
        }
    }

    if (propertyId == UIA_WindowWindowVisualStatePropertyId && v.vt == VT_I4) {
        switch (v.lVal) {
        case WindowVisualState_Normal:    return "Normal";
        case WindowVisualState_Maximized: return "Maximized";
        case WindowVisualState_Minimized: return "Minimized";
        default: break;
        }
    }

    if (propertyId == UIA_NativeWindowHandlePropertyId && v.vt == VT_I4) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llX",
                 static_cast<unsigned long long>(static_cast<unsigned int>(v.lVal)));
        return buf;
    }

    return raw;
}

// Properties that become first-class Element fields rather than entries in the
// property map, so they are not duplicated in both places.
bool is_promoted_property(long propertyId) {
    return propertyId == UIA_NamePropertyId ||
           propertyId == UIA_ClassNamePropertyId ||
           propertyId == UIA_BoundingRectanglePropertyId;
}

struct WalkContext {
    wil::com_ptr<IUIAutomation> automation;
    wil::com_ptr<IUIAutomationCacheRequest> cacheRequest;
    std::vector<long> properties;
    // Properties named explicitly via --uia-props. These bypass the unset-value
    // suppression: if a caller asked for a property by name, report it even when
    // it holds its default.
    std::set<long> requestedProperties;
    clock_type::time_point deadline;
    bool hasDeadline = false;
    bool truncated = false;
    int maxDepth = -1;

    bool expired() {
        if (!hasDeadline)
            return false;
        if (clock_type::now() >= deadline) {
            truncated = true;
            return true;
        }
        return false;
    }
};

void apply_cached_properties(WalkContext& ctx, IUIAutomationElement* element, Element& out) {
    // Patterns first: pattern support gates which pattern-backed properties are
    // meaningful, so it has to be known before the property loop runs.
    std::string supportedPatterns;
    std::set<long> supported;
    for (long patternId : uia_probed_pattern_ids()) {
        // Availability comes from the cached pattern set, so this costs no extra
        // cross-process calls: it was part of the batched request.
        wil::com_ptr<IUnknown> pattern;
        if (FAILED(element->GetCachedPattern(patternId, &pattern)) || !pattern)
            continue;
        supported.insert(patternId);
        auto name = uia_pattern_name(patternId);
        if (name.empty())
            continue;
        if (!supportedPatterns.empty())
            supportedPatterns += ',';
        supportedPatterns += name;
    }

    for (long propertyId : ctx.properties) {
        // Skip a pattern-backed property when the element does not support the
        // owning pattern; UIA would otherwise answer with a meaningless default.
        const long owner = uia_property_owner_pattern(propertyId);
        if (owner != 0 && supported.find(owner) == supported.end())
            continue;

        wil::unique_variant value;
        // A provider may simply not answer for a given property; that is normal
        // and must not abort the element, let alone the walk.
        if (FAILED(element->GetCachedPropertyValue(propertyId, &value)))
            continue;

        if (propertyId == UIA_BoundingRectanglePropertyId) {
            // BoundingRectangle is a 4-double SAFEARRAY: left, top, width, height.
            if (value.vt == (VT_ARRAY | VT_R8) && value.parray &&
                SafeArrayGetDim(value.parray) == 1) {
                LONG lower = 0, upper = -1;
                if (SUCCEEDED(SafeArrayGetLBound(value.parray, 1, &lower)) &&
                    SUCCEEDED(SafeArrayGetUBound(value.parray, 1, &upper)) &&
                    upper - lower == 3) {
                    double rect[4] = {};
                    bool ok = true;
                    for (LONG i = 0; i < 4 && ok; ++i) {
                        LONG index = lower + i;
                        ok = SUCCEEDED(SafeArrayGetElement(value.parray, &index, &rect[i]));
                    }
                    if (ok) {
                        out.bounds = {static_cast<int>(rect[0]), static_cast<int>(rect[1]),
                                      static_cast<int>(rect[2]), static_cast<int>(rect[3])};
                    }
                }
                continue;
            }
        }

        const auto raw = variant_to_string(value);
        if (raw.empty())
            continue;

        if (propertyId == UIA_NamePropertyId) {
            out.text = raw;
            continue;
        }
        if (propertyId == UIA_ClassNamePropertyId) {
            out.className = raw;
            continue;
        }
        if (propertyId == UIA_NativeWindowHandlePropertyId && value.vt == VT_I4 && value.lVal != 0)
            out.nativeHandle = static_cast<uintptr_t>(static_cast<unsigned int>(value.lVal));
        if (propertyId == UIA_ControlTypePropertyId && value.vt == VT_I4)
            out.type = uia_control_type_name(value.lVal);

        if (is_promoted_property(propertyId))
            continue;

        auto name = uia_property_name(propertyId);
        if (name.empty())
            continue;

        auto rendered = humanize(propertyId, value, raw);
        if (ctx.requestedProperties.find(propertyId) == ctx.requestedProperties.end() &&
            uia_property_value_is_unset(propertyId, rendered))
            continue;
        out.properties[name] = std::move(rendered);
    }

    if (!supportedPatterns.empty())
        out.properties["SupportedPatterns"] = supportedPatterns;

    if (out.type.empty())
        out.type = "Element";
    out.framework = "uia";
}

Element build_from_cached(WalkContext& ctx, IUIAutomationElement* element, int depth) {
    Element out;
    apply_cached_properties(ctx, element, out);

    if (ctx.maxDepth >= 0 && depth >= ctx.maxDepth)
        return out;
    if (ctx.expired())
        return out;

    wil::com_ptr<IUIAutomationElementArray> children;
    if (FAILED(element->GetCachedChildren(&children)) || !children)
        return out;

    int count = 0;
    if (FAILED(children->get_Length(&count)))
        return out;

    for (int i = 0; i < count; ++i) {
        if (ctx.expired())
            break;
        wil::com_ptr<IUIAutomationElement> child;
        if (FAILED(children->GetElement(i, &child)) || !child)
            continue;
        out.children.push_back(build_from_cached(ctx, child.get(), depth + 1));
    }

    return out;
}

// Build the batched cache request. This is the single most important
// performance decision in the provider: without it, every property on every
// node is a separate cross-process call, and a moderately sized window takes
// tens of seconds. With it the whole subtree arrives in essentially one round
// trip.
HRESULT make_cache_request(IUIAutomation* automation, const UiaOptions& options,
                           const std::vector<long>& properties,
                           IUIAutomationCacheRequest** out) {
    wil::com_ptr<IUIAutomationCacheRequest> request;
    RETURN_IF_FAILED(automation->CreateCacheRequest(&request));
    RETURN_IF_FAILED(request->put_TreeScope(TreeScope_Subtree));
    RETURN_IF_FAILED(request->put_AutomationElementMode(AutomationElementMode_Full));

    for (long propertyId : properties)
        LOG_IF_FAILED(request->AddProperty(propertyId));
    for (long patternId : uia_probed_pattern_ids())
        LOG_IF_FAILED(request->AddPattern(patternId));

    wil::com_ptr<IUIAutomationCondition> filter;
    switch (options.view) {
    case UiaView::raw:
        RETURN_IF_FAILED(automation->CreateTrueCondition(&filter));
        break;
    case UiaView::control:
        RETURN_IF_FAILED(automation->get_ControlViewCondition(&filter));
        break;
    case UiaView::content:
        RETURN_IF_FAILED(automation->get_ContentViewCondition(&filter));
        break;
    }
    RETURN_IF_FAILED(request->put_TreeFilter(filter.get()));

    *out = request.detach();
    return S_OK;
}

std::vector<long> resolve_properties(const UiaOptions& options,
                                     std::set<long>* requested = nullptr) {
    auto properties = uia_core_property_ids();
    for (const auto& name : options.extraProperties) {
        const long id = uia_property_id(name);
        if (id == 0) {
            fprintf(stderr, "lvt: unknown UIA property '%s'\n", name.c_str());
            continue;
        }
        if (requested)
            requested->insert(id);
        if (std::find(properties.begin(), properties.end(), id) == properties.end())
            properties.push_back(id);
    }
    return properties;
}

HRESULT create_automation(IUIAutomation** out) {
    // CUIAutomation8 gives the IUIAutomation6 generation, which supports
    // per-call timeouts and connection-recovery behaviour. Fall back to the
    // original CLSID on older systems.
    wil::com_ptr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation));
    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        RETURN_IF_FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&automation)));
    }

    // Keep a slow or wedged provider from stalling the whole walk.
    if (auto automation2 = automation.try_query<IUIAutomation2>()) {
        LOG_IF_FAILED(automation2->put_ConnectionTimeout(2000));
        LOG_IF_FAILED(automation2->put_TransactionTimeout(2000));
    }

    *out = automation.detach();
    return S_OK;
}

HRESULT build_tree_on_mta(HWND hwnd, const UiaOptions& options,
                          Element& out, bool& truncated) {
    wil::com_ptr<IUIAutomation> automation;
    RETURN_IF_FAILED(create_automation(&automation));

    wil::com_ptr<IUIAutomationElement> root;
    RETURN_IF_FAILED(automation->ElementFromHandle(hwnd, &root));
    RETURN_HR_IF_NULL(E_FAIL, root.get());

    std::set<long> requested;
    auto properties = resolve_properties(options, &requested);

    wil::com_ptr<IUIAutomationCacheRequest> request;
    RETURN_IF_FAILED(make_cache_request(automation.get(), options, properties, &request));

    wil::com_ptr<IUIAutomationElement> cachedRoot;
    RETURN_IF_FAILED(root->BuildUpdatedCache(request.get(), &cachedRoot));
    RETURN_HR_IF_NULL(E_FAIL, cachedRoot.get());

    WalkContext ctx;
    ctx.automation = automation;
    ctx.cacheRequest = request;
    ctx.properties = std::move(properties);
    ctx.requestedProperties = std::move(requested);
    ctx.maxDepth = options.maxDepth;
    if (options.timeoutMs > 0) {
        ctx.hasDeadline = true;
        ctx.deadline = clock_type::now() + std::chrono::milliseconds(options.timeoutMs);
    }

    out = build_from_cached(ctx, cachedRoot.get(), 0);
    truncated = ctx.truncated;
    return S_OK;
}

HRESULT element_from_point_on_mta(POINT screenPoint, const UiaOptions& options, Element& out) {
    wil::com_ptr<IUIAutomation> automation;
    RETURN_IF_FAILED(create_automation(&automation));

    wil::com_ptr<IUIAutomationElement> element;
    RETURN_IF_FAILED(automation->ElementFromPoint(screenPoint, &element));
    RETURN_HR_IF_NULL(E_FAIL, element.get());

    std::set<long> requested;
    auto properties = resolve_properties(options, &requested);

    // Scope to the element itself; a hit test wants one node, not a subtree.
    wil::com_ptr<IUIAutomationCacheRequest> request;
    RETURN_IF_FAILED(automation->CreateCacheRequest(&request));
    RETURN_IF_FAILED(request->put_TreeScope(TreeScope_Element));
    RETURN_IF_FAILED(request->put_AutomationElementMode(AutomationElementMode_Full));
    for (long propertyId : properties)
        LOG_IF_FAILED(request->AddProperty(propertyId));
    for (long patternId : uia_probed_pattern_ids())
        LOG_IF_FAILED(request->AddPattern(patternId));

    wil::com_ptr<IUIAutomationElement> cached;
    RETURN_IF_FAILED(element->BuildUpdatedCache(request.get(), &cached));
    RETURN_HR_IF_NULL(E_FAIL, cached.get());

    WalkContext ctx;
    ctx.automation = automation;
    ctx.cacheRequest = request;
    ctx.properties = std::move(properties);
    ctx.requestedProperties = std::move(requested);
    ctx.maxDepth = 0;

    out = build_from_cached(ctx, cached.get(), 0);
    return S_OK;
}

// UIA clients belong in an MTA. screenshot.cpp initializes an STA on the calling
// thread, and a thread cannot be in both, so all UIA work is marshalled onto a
// dedicated MTA thread. This also serialises access to the client.
template <typename Fn>
HRESULT run_on_mta(Fn&& fn) {
    HRESULT result = E_FAIL;
    std::thread worker([&] {
        auto uninit = wil::CoInitializeEx_failfast(COINIT_MULTITHREADED);
        result = fn();
    });
    worker.join();
    return result;
}

} // namespace

std::string format_runtime_id(const std::vector<int>& runtimeId) {
    std::string out;
    for (int part : runtimeId) {
        if (!out.empty())
            out += '.';
        out += std::to_string(part);
    }
    return out;
}

bool parse_runtime_id(const std::string& text, std::vector<int>& out) {
    if (text.empty())
        return false;
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        const size_t dot = text.find('.', start);
        const auto piece = text.substr(start, dot == std::string::npos ? std::string::npos
                                                                       : dot - start);
        if (piece.empty())
            return false;
        for (char c : piece) {
            if (c < '0' || c > '9')
                return false;
        }
        try {
            out.push_back(std::stoi(piece));
        } catch (...) {
            return false;
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return !out.empty();
}

std::optional<Element> UiaProvider::build(HWND hwnd, const UiaOptions& options, bool* truncated) {
    Element root;
    bool wasTruncated = false;

    const HRESULT hr = run_on_mta([&] {
        return build_tree_on_mta(hwnd, options, root, wasTruncated);
    });

    if (truncated)
        *truncated = wasTruncated;

    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        return std::nullopt;
    }
    if (wasTruncated) {
        fprintf(stderr, "lvt: UIA walk hit the %d ms deadline; tree is partial\n",
                options.timeoutMs);
    }
    return root;
}

std::optional<Element> UiaProvider::element_from_point(POINT screenPoint,
                                                       const UiaOptions& options) {
    Element element;
    const HRESULT hr = run_on_mta([&] {
        return element_from_point_on_mta(screenPoint, options, element);
    });
    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        return std::nullopt;
    }
    return element;
}

std::optional<Element> UiaProvider::element_by_runtime_id(HWND hwnd, const std::string& runtimeId,
                                                          const UiaOptions& options) {
    std::vector<int> parsed;
    if (!parse_runtime_id(runtimeId, parsed))
        return std::nullopt;

    // Walk the tree and match on the RuntimeId property rather than using
    // ElementFromIUIAutomationId, which does not exist; a scoped walk is the
    // supported way to re-find an element by runtime id.
    auto tree = build(hwnd, options);
    if (!tree)
        return std::nullopt;

    const std::string target = format_runtime_id(parsed);
    std::vector<const Element*> stack{&*tree};
    while (!stack.empty()) {
        const Element* current = stack.back();
        stack.pop_back();
        auto it = current->properties.find("RuntimeId");
        if (it != current->properties.end() && it->second == target)
            return *current;
        for (const auto& child : current->children)
            stack.push_back(&child);
    }
    return std::nullopt;
}

} // namespace lvt
