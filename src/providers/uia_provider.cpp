#include "uia_provider.h"
#include "../debug.h"

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <oleacc.h>
#include <UIAutomation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lvt {
namespace {

using clock_type = std::chrono::steady_clock;
static constexpr DWORD kUiaDefaultTransactionTimeoutMs = 20000;
static constexpr DWORD kUiaConnectionTimeoutCapMs = 2000;

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
        std::vector<int> parts;
        parts.reserve(static_cast<size_t>((std::max)(0L, upper - lower + 1)));
        for (LONG i = lower; i <= upper; ++i) {
            int element = 0;
            if (FAILED(SafeArrayGetElement(sa, &i, &element)))
                return {};
            parts.push_back(element);
        }
        // Same formatting the uia:<RuntimeId> reference parser expects, so the
        // two cannot drift apart.
        return format_runtime_id(parts);
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

// A few properties read better as names than as raw enum numbers or handles.
std::string humanize(long propertyId, const VARIANT& v, const std::string& raw) {
    if (v.vt == VT_I4 && uia_property_is_enum(propertyId))
        return uia_enum_value_name(propertyId, v.lVal);

    if (propertyId == UIA_CulturePropertyId && v.vt == VT_I4)
        return uia_culture_name(v.lVal);

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
           propertyId == UIA_ControlTypePropertyId ||
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
    // UIA's sentinel for "this element does not support this property". See
    // apply_cached_properties for why identity against this matters.
    wil::com_ptr<IUnknown> notSupported;
    clock_type::time_point deadline;
    bool hasDeadline = false;
    bool truncated = false;

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
    std::string supportedPatterns;
    for (long patternId : uia_probed_pattern_ids()) {
        // Availability comes from the cached pattern set, so this costs no extra
        // cross-process calls: it was part of the batched request.
        wil::com_ptr<IUnknown> pattern;
        if (FAILED(element->GetCachedPattern(patternId, &pattern)) || !pattern)
            continue;
        auto name = uia_pattern_name(patternId);
        if (name.empty())
            continue;
        if (!supportedPatterns.empty())
            supportedPatterns += ',';
        supportedPatterns += name;
    }

    for (long propertyId : ctx.properties) {
        const auto name = uia_property_name(propertyId);

        // Pattern-backed properties are named "Pattern.Member"; that naming is
        // the rule, asserted by the unit tests. Only they need the Ex form:
        //
        // GetCachedPropertyValue substitutes the property type's *default* when
        // the element does not support the owning pattern, so a Window answers
        // Toggle.ToggleState with 2 (ToggleState_Indeterminate) as readily as a
        // real checkbox does. GetCachedPropertyValueEx with ignoreDefaultValue
        // returns UIA's reserved "not supported" object instead — the
        // provider-authoritative signal, and it needs no property-to-pattern table.
        //
        // Core properties deliberately keep the plain form: for those, UIA also
        // reports "not supported" whenever a provider did not explicitly set the
        // value, which would silently drop useful state such as IsControlElement.
        const bool patternBacked = name.find('.') != std::string::npos;

        wil::unique_variant value;
        const HRESULT hr = patternBacked
            ? element->GetCachedPropertyValueEx(propertyId, TRUE, &value)
            : element->GetCachedPropertyValue(propertyId, &value);
        // A provider may simply not answer; that is normal and must not abort
        // the element, let alone the walk.
        if (FAILED(hr))
            continue;
        if (patternBacked && value.vt == VT_UNKNOWN && value.punkVal == ctx.notSupported.get())
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
        // An empty string is normally just absence, but for a pattern-backed
        // property it is real information: the element supports Value and the
        // value is empty, which a consumer must be able to tell apart from
        // "no Value pattern". Only the Ex accessor is trusted to have already
        // filtered unsupported properties, so this is safe.
        if (raw.empty() && !patternBacked)
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

        if (name.empty())
            continue;

        auto rendered = humanize(propertyId, value, raw);
        // Sentinels are expressed in raw form, so the check happens before
        // humanizing: LiveSetting 0 renders as "Off" and LandmarkType 0 as
        // "LandmarkType(0)", neither of which would ever match a sentinel.
        if (ctx.requestedProperties.find(propertyId) == ctx.requestedProperties.end() &&
            uia_property_value_is_unset(propertyId, raw))
            continue;
        out.properties[name] = std::move(rendered);
    }

    if (!supportedPatterns.empty())
        out.properties["SupportedPatterns"] = supportedPatterns;

    if (out.type.empty())
        out.type = "Element";
    out.framework = "uia";
}

Element build_from_cached(WalkContext& ctx, IUIAutomationElement* element) {
    Element out;
    apply_cached_properties(ctx, element, out);

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
        out.children.push_back(build_from_cached(ctx, child.get()));
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

void apply_automation_timeouts(IUIAutomation* automation, const UiaOptions& options) {
    if (!automation)
        return;

    // A fresh one-shot client can get "UIA's default 20s transaction timeout"
    // by simply never calling put_TransactionTimeout at all. A REUSED client
    // cannot: once one walk tightened or widened the timeout, that value sticks
    // to the same automation object for the next walk unless lvt actively puts
    // it back. Making the effective defaults explicit here keeps the persistent
    // path behaviorally aligned with the one-shot path instead of letting one
    // call's timeout leak into the next.
    const DWORD transactionTimeoutMs = options.timeoutMs > 0
        ? static_cast<DWORD>(options.timeoutMs)
        : kUiaDefaultTransactionTimeoutMs;

    wil::com_ptr<IUIAutomation> automationRef;
    automationRef = automation;
    if (auto automation2 = automationRef.try_query<IUIAutomation2>()) {
        LOG_IF_FAILED(automation2->put_TransactionTimeout(transactionTimeoutMs));
        // Connecting should never need the whole budget; cap it so an
        // unreachable provider fails fast instead of consuming the deadline.
        LOG_IF_FAILED(automation2->put_ConnectionTimeout(
            (std::min)(transactionTimeoutMs, kUiaConnectionTimeoutCapMs)));
    }
}

HRESULT create_automation(const UiaOptions& options, IUIAutomation** out) {
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

    // This is what actually bounds a wedged target. Every cross-process call
    // happens inside BuildUpdatedCache, so the deadline checked between
    // elements later only limits the cheap in-process walk of the materialised
    // cache. Driving the transaction timeout from --uia-timeout is what makes
    // that flag mean what it says.
    apply_automation_timeouts(automation.get(), options);

    *out = automation.detach();
    return S_OK;
}

HRESULT build_tree_with_automation(IUIAutomation* automation, HWND hwnd,
                                   const UiaOptions& options,
                                   Element& out, bool& truncated) {
    wil::com_ptr<IUIAutomationElement> root;
    RETURN_IF_FAILED(automation->ElementFromHandle(hwnd, &root));
    RETURN_HR_IF_NULL(E_FAIL, root.get());

    std::set<long> requested;
    auto properties = resolve_properties(options, &requested);

    wil::com_ptr<IUIAutomationCacheRequest> request;
    RETURN_IF_FAILED(make_cache_request(automation, options, properties, &request));

    wil::com_ptr<IUIAutomationElement> cachedRoot;
    RETURN_IF_FAILED(root->BuildUpdatedCache(request.get(), &cachedRoot));
    RETURN_HR_IF_NULL(E_FAIL, cachedRoot.get());

    WalkContext ctx;
    ctx.automation = automation;
    ctx.cacheRequest = request;
    ctx.properties = std::move(properties);
    ctx.requestedProperties = std::move(requested);
    LOG_IF_FAILED(automation->get_ReservedNotSupportedValue(ctx.notSupported.put()));
    if (options.timeoutMs > 0) {
        ctx.hasDeadline = true;
        ctx.deadline = clock_type::now() + std::chrono::milliseconds(options.timeoutMs);
    }

    out = build_from_cached(ctx, cachedRoot.get());
    truncated = ctx.truncated;
    if (ctx.truncated) {
        // The consumer of this tree is a machine. A silently shortened tree is
        // indistinguishable from a complete one, so the marker has to travel in
        // the document, not only on stderr.
        out.properties["Truncated"] = "true";
    }
    return S_OK;
}

HRESULT build_tree_on_mta(HWND hwnd, const UiaOptions& options,
                          Element& out, bool& truncated) {
    wil::com_ptr<IUIAutomation> automation;
    RETURN_IF_FAILED(create_automation(options, &automation));
    return build_tree_with_automation(automation.get(), hwnd, options, out, truncated);
}

// UIA clients belong in an MTA. screenshot.cpp initializes an STA on the calling
// thread, and a thread cannot be in both, so all UIA work is marshalled onto a
// dedicated MTA thread.
//
// One-shot callers still create and release their COM objects inside `fn`.
// UiaConnection is the deliberate exception: it creates IUIAutomation once on
// one run_on_mta call and reuses that pointer on later run_on_mta calls. That
// is still the SAME apartment because COINIT_MULTITHREADED joins the single,
// process-wide MTA rather than inventing a per-thread apartment.
//
// The body is wrapped in CATCH_RETURN because an exception escaping a thread
// function calls std::terminate: building the Element tree allocates freely, so
// a bad_alloc on a large UI would otherwise take the whole process down instead
// of failing the walk. Same rule as the extern "C" boundaries.
template <typename Fn>
HRESULT run_on_mta(Fn&& fn) {
    HRESULT result = E_FAIL;
    std::thread worker([&] {
        result = [&]() -> HRESULT {
            try {
                auto uninit = wil::CoInitializeEx_failfast(COINIT_MULTITHREADED);
                return fn();
            }
            CATCH_RETURN();
        }();
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
        // Components are signed: RuntimeIds derived from an HWND with the high
        // bit set are negative, and format_runtime_id emits them with a '-'.
        const size_t digitsFrom = piece[0] == '-' ? 1 : 0;
        if (piece.size() == digitsFrom)
            return false;
        for (size_t i = digitsFrom; i < piece.size(); ++i) {
            if (piece[i] < '0' || piece[i] > '9')
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

struct UiaConnection::State {
    std::mutex mutex;
    wil::com_ptr<IUIAutomation> automation;
    CO_MTA_USAGE_COOKIE mtaCookie = nullptr;
};

UiaConnection::UiaConnection(HWND hwnd)
    : m_hwnd(hwnd), m_state(std::make_unique<State>()) {
}

std::shared_ptr<UiaConnection> UiaConnection::connect(HWND hwnd) {
    auto connection = std::shared_ptr<UiaConnection>(new UiaConnection(hwnd));
    const HRESULT hr = run_on_mta([&]() -> HRESULT {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        RETURN_IF_FAILED(CoIncrementMTAUsage(&cookie));

        wil::com_ptr<IUIAutomation> automation;
        UiaOptions connectOptions;
        // connect() is not itself a tree walk; the very next get_tree call
        // re-applies that walk's own timeout anyway, so create the client with
        // the default-effective setting rather than baking in an arbitrary
        // caller-specific budget up front.
        connectOptions.timeoutMs = 0;
        const HRESULT createHr = create_automation(connectOptions, &automation);
        if (FAILED(createHr)) {
            CoDecrementMTAUsage(cookie);
            RETURN_HR(createHr);
        }

        std::lock_guard<std::mutex> lock(connection->m_state->mutex);
        connection->m_state->automation = std::move(automation);
        connection->m_state->mtaCookie = cookie;
        return S_OK;
    });

    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        return {};
    }
    return connection;
}

UiaConnection::~UiaConnection() {
    wil::com_ptr<IUIAutomation> automation;
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        automation = std::move(m_state->automation);
        cookie = m_state->mtaCookie;
        m_state->mtaCookie = nullptr;
    }
    if (automation) {
        // connect() took an MTA-usage cookie specifically so the process-wide
        // MTA stays alive even between our short-lived worker threads. Release
        // the automation object before dropping that cookie, otherwise the last
        // CoDecrementMTAUsage could tear the apartment down while this pointer
        // still belongs to it.
        (void)run_on_mta([&]() -> HRESULT {
            automation.reset();
            return S_OK;
        });
    }
    if (cookie)
        CoDecrementMTAUsage(cookie);
}

bool UiaConnection::get_tree(Element& root, bool fastProperties,
                             const std::string& /*providerOption*/) {
    (void)fastProperties;
    return get_tree_with_options(root, UiaOptions{});
}

bool UiaConnection::get_tree_with_options(Element& root, const UiaOptions& options,
                                          bool* truncated) {
    if (truncated)
        *truncated = false;

    // One live walk at a time against the reused client. Parallel reads buy
    // nothing here — the target's UI thread still answers them serially — and
    // holding the lock across the whole run also keeps teardown from releasing
    // the shared automation object while this call is still using it. Read
    // only the raw pointer here; even com_ptr's AddRef would execute on the
    // caller's thread, which may be STA or not in COM at all.
    std::unique_lock<std::mutex> lock(m_state->mutex);
    IUIAutomation* automation = m_state->automation.get();
    if (!automation)
        return false;

    Element built;
    bool wasTruncated = false;
    const HWND hwnd = m_hwnd;
    const HRESULT hr = run_on_mta([automation, hwnd, &options, &built, &wasTruncated]() -> HRESULT {
        // connect() created this automation object on one thread that had
        // joined the process-wide MTA. Every get_tree_with_options call joins
        // that same single MTA before touching it, so the raw pointer stays in
        // one apartment throughout its whole life and needs no marshal/proxy.
        // connect() also took an MTA-usage cookie, so that apartment survives
        // between our short-lived worker threads instead of being torn down
        // when the creating thread exits.
        apply_automation_timeouts(automation, options);
        return build_tree_with_automation(automation, hwnd, options, built, wasTruncated);
    });
    lock.unlock();

    if (truncated)
        *truncated = wasTruncated;

    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        return false;
    }
    if (wasTruncated) {
        fprintf(stderr, "lvt: UIA walk hit the %d ms deadline; tree is partial\n",
                options.timeoutMs);
    }
    root = std::move(built);
    return true;
}

std::vector<ConnectionEvent> UiaConnection::poll_events() {
    return {};
}

bool UiaConnection::is_alive() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->automation != nullptr;
}

} // namespace lvt
