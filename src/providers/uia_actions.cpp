#include "uia_actions.h"
#include "../debug.h"
#include "../element_key.h"
#include "../input.h"
#include "../tree_builder.h"

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <oleacc.h>
#include <UIAutomation.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace lvt {
namespace {

// A UIA client has thread affinity to its apartment, so every call here runs on
// a dedicated MTA thread for the same reason the walk does.
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

HRESULT create_automation(const UiaOptions& options, IUIAutomation** out) {
    wil::com_ptr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation));
    if (FAILED(hr)) {
        LOG_IF_FAILED(hr);
        RETURN_IF_FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&automation)));
    }
    if (options.timeoutMs > 0) {
        if (auto automation2 = automation.try_query<IUIAutomation2>()) {
            LOG_IF_FAILED(automation2->put_TransactionTimeout(
                static_cast<DWORD>(options.timeoutMs)));
            LOG_IF_FAILED(automation2->put_ConnectionTimeout(
                static_cast<DWORD>((std::min)(options.timeoutMs, 2000))));
        }
    }
    *out = automation.detach();
    return S_OK;
}

// Re-find the live element by RuntimeId. The walk produces plain data, so
// acting on an element means locating it again; RuntimeId is the handle UIA
// provides for exactly this.
HRESULT find_by_runtime_id(IUIAutomation* automation, HWND hwnd,
                           const std::vector<int>& runtimeId,
                           IUIAutomationElement** out) {
    wil::com_ptr<IUIAutomationElement> root;
    RETURN_IF_FAILED(automation->ElementFromHandle(hwnd, &root));
    RETURN_HR_IF_NULL(E_FAIL, root.get());

    wil::unique_variant condition;
    SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(runtimeId.size()));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, array);
    condition.vt = VT_ARRAY | VT_I4;
    condition.parray = array;  // unique_variant owns it from here
    for (LONG i = 0; i < static_cast<LONG>(runtimeId.size()); ++i)
        RETURN_IF_FAILED(SafeArrayPutElement(array, &i, (void*)&runtimeId[static_cast<size_t>(i)]));

    wil::com_ptr<IUIAutomationCondition> byRuntimeId;
    RETURN_IF_FAILED(automation->CreatePropertyCondition(
        UIA_RuntimeIdPropertyId, condition, &byRuntimeId));

    wil::com_ptr<IUIAutomationElement> found;
    RETURN_IF_FAILED(root->FindFirst(TreeScope_Subtree, byRuntimeId.get(), &found));
    if (!found) {
        // The root itself is not covered by a Subtree search on some providers.
        wil::unique_variant rootId;
        if (SUCCEEDED(root->GetCurrentPropertyValue(UIA_RuntimeIdPropertyId, &rootId)) &&
            rootId.vt == (VT_ARRAY | VT_I4)) {
            found = root;
        }
    }
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), found.get());
    *out = found.detach();
    return S_OK;
}

POINT element_center(const Element& element) {
    return POINT{element.bounds.x + element.bounds.width / 2,
                 element.bounds.y + element.bounds.height / 2};
}

bool has_pattern(IUIAutomationElement* element, PATTERNID pattern) {
    wil::com_ptr<IUnknown> unknown;
    return SUCCEEDED(element->GetCurrentPattern(pattern, &unknown)) && unknown;
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

// --- individual actions -------------------------------------------------
// An attempt distinguishes the two ways a pattern can decline: the element does
// not expose it at all, or it exposes it and the call failed. Chained actions
// like click need to move on in both cases; single-pattern actions like
// --invoke need to report which one happened.
struct PatternAttempt {
    bool supported = false;
    HRESULT hr = S_OK;

    bool succeeded() const { return supported && SUCCEEDED(hr); }
};

PatternAttempt try_invoke(IUIAutomationElement* element, std::string& method) {
    wil::com_ptr<IUIAutomationInvokePattern> invoke;
    if (FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) ||
        !invoke)
        return {};
    PatternAttempt attempt{true, invoke->Invoke()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "InvokePattern";
    return attempt;
}

PatternAttempt try_legacy_default_action(IUIAutomationElement* element, std::string& method) {
    wil::com_ptr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (FAILED(element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                            IID_PPV_ARGS(&legacy))) || !legacy)
        return {};
    // Nearly everything exposes LegacyIAccessible, but only some have a default
    // action that does anything, so a failure here is routine.
    PatternAttempt attempt{true, legacy->DoDefaultAction()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "LegacyIAccessible.DoDefaultAction";
    return attempt;
}

PatternAttempt try_toggle(IUIAutomationElement* element, std::string& method) {
    wil::com_ptr<IUIAutomationTogglePattern> toggle;
    if (FAILED(element->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) ||
        !toggle)
        return {};
    PatternAttempt attempt{true, toggle->Toggle()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "TogglePattern";
    return attempt;
}

PatternAttempt try_set_value(IUIAutomationElement* element, const std::string& text,
                             std::string& method) {
    wil::com_ptr<IUIAutomationValuePattern> value;
    if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) ||
        !value)
        return {};
    BOOL readOnly = FALSE;
    if (SUCCEEDED(value->get_CurrentIsReadOnly(&readOnly)) && readOnly)
        return {};  // present but not writable: same as unavailable to a caller

    wil::unique_bstr bstr(SysAllocString(widen(text).c_str()));
    if (!bstr)
        return {true, E_OUTOFMEMORY};
    PatternAttempt attempt{true, value->SetValue(bstr.get())};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "ValuePattern";
    return attempt;
}

PatternAttempt try_expand_collapse(IUIAutomationElement* element, bool expand,
                                   std::string& method) {
    wil::com_ptr<IUIAutomationExpandCollapsePattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                            IID_PPV_ARGS(&pattern))) || !pattern)
        return {};
    PatternAttempt attempt{true, expand ? pattern->Expand() : pattern->Collapse()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "ExpandCollapsePattern";
    return attempt;
}

PatternAttempt try_select(IUIAutomationElement* element, std::string& method) {
    wil::com_ptr<IUIAutomationSelectionItemPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                            IID_PPV_ARGS(&pattern))) || !pattern)
        return {};
    PatternAttempt attempt{true, pattern->Select()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "SelectionItemPattern";
    return attempt;
}

PatternAttempt try_scroll(IUIAutomationElement* element, const std::string& direction,
                          int amount, std::string& method) {
    wil::com_ptr<IUIAutomationScrollPattern> scroll;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&scroll))) &&
        scroll) {
        auto horizontal = ScrollAmount_NoAmount;
        auto vertical = ScrollAmount_NoAmount;
        if (direction == "up")    vertical = ScrollAmount_SmallDecrement;
        if (direction == "down")  vertical = ScrollAmount_SmallIncrement;
        if (direction == "left")  horizontal = ScrollAmount_SmallDecrement;
        if (direction == "right") horizontal = ScrollAmount_SmallIncrement;
        if (horizontal == ScrollAmount_NoAmount && vertical == ScrollAmount_NoAmount)
            return {true, E_INVALIDARG};

        HRESULT hr = S_OK;
        for (int i = 0; i < (std::max)(1, amount) && SUCCEEDED(hr); ++i)
            hr = scroll->Scroll(horizontal, vertical);
        LOG_IF_FAILED(hr);
        PatternAttempt attempt{true, hr};
        if (attempt.succeeded())
            method = "ScrollPattern";
        return attempt;
    }

    // Not scrollable itself, but it may be an item inside something that is.
    wil::com_ptr<IUIAutomationScrollItemPattern> scrollItem;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ScrollItemPatternId,
                                               IID_PPV_ARGS(&scrollItem))) && scrollItem) {
        PatternAttempt attempt{true, scrollItem->ScrollIntoView()};
        LOG_IF_FAILED(attempt.hr);
        if (attempt.succeeded())
            method = "ScrollItemPattern.ScrollIntoView";
        return attempt;
    }
    return {};
}

// A virtualized item does not exist as a real element until it is realized, so
// nothing can be done to it. Realizing first is transparent and cheap: an
// already-real element simply does not expose the pattern.
bool realize_if_virtualized(IUIAutomationElement* element) {
    wil::com_ptr<IUIAutomationVirtualizedItemPattern> virtualized;
    if (FAILED(element->GetCurrentPatternAs(UIA_VirtualizedItemPatternId,
                                            IID_PPV_ARGS(&virtualized))) || !virtualized)
        return false;
    LOG_IF_FAILED(virtualized->Realize());
    return true;
}

PatternAttempt try_set_range_value(IUIAutomationElement* element, const std::string& text,
                                   std::string& method) {
    wil::com_ptr<IUIAutomationRangeValuePattern> range;
    if (FAILED(element->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) ||
        !range)
        return {};
    BOOL readOnly = FALSE;
    if (SUCCEEDED(range->get_CurrentIsReadOnly(&readOnly)) && readOnly)
        return {};

    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0')
        return {};  // not a number, so RangeValue is not what the caller meant

    PatternAttempt attempt{true, range->SetValue(value)};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "RangeValuePattern";
    return attempt;
}

PatternAttempt try_change_selection(IUIAutomationElement* element, bool add,
                                    std::string& method) {
    wil::com_ptr<IUIAutomationSelectionItemPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                            IID_PPV_ARGS(&pattern))) || !pattern)
        return {};
    PatternAttempt attempt{true, add ? pattern->AddToSelection()
                                     : pattern->RemoveFromSelection()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = add ? "SelectionItemPattern.AddToSelection"
                     : "SelectionItemPattern.RemoveFromSelection";
    return attempt;
}

PatternAttempt try_select_text(IUIAutomationElement* element, const std::string& needle,
                               std::string& method) {
    wil::com_ptr<IUIAutomationTextPattern> text;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) || !text)
        return {};

    wil::com_ptr<IUIAutomationTextRange> document;
    if (FAILED(text->get_DocumentRange(&document)) || !document)
        return {true, E_FAIL};

    if (needle.empty()) {
        PatternAttempt attempt{true, document->Select()};
        LOG_IF_FAILED(attempt.hr);
        if (attempt.succeeded())
            method = "TextPattern.SelectAll";
        return attempt;
    }

    wil::unique_bstr wanted(SysAllocString(widen(needle).c_str()));
    if (!wanted)
        return {true, E_OUTOFMEMORY};

    wil::com_ptr<IUIAutomationTextRange> found;
    if (FAILED(document->FindText(wanted.get(), FALSE, FALSE, &found)) || !found)
        return {true, HRESULT_FROM_WIN32(ERROR_NOT_FOUND)};

    PatternAttempt attempt{true, found->Select()};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "TextPattern.Select";
    return attempt;
}

PatternAttempt try_window_action(IUIAutomationElement* element, ActionKind kind,
                                 std::string& method) {
    wil::com_ptr<IUIAutomationWindowPattern> window;
    if (FAILED(element->GetCurrentPatternAs(UIA_WindowPatternId, IID_PPV_ARGS(&window))) ||
        !window)
        return {};

    if (kind == ActionKind::windowClose) {
        PatternAttempt attempt{true, window->Close()};
        LOG_IF_FAILED(attempt.hr);
        if (attempt.succeeded())
            method = "WindowPattern.Close";
        return attempt;
    }

    WindowVisualState state = WindowVisualState_Normal;
    if (kind == ActionKind::windowMinimize) state = WindowVisualState_Minimized;
    if (kind == ActionKind::windowMaximize) state = WindowVisualState_Maximized;

    BOOL canDo = TRUE;
    if (kind == ActionKind::windowMinimize)
        LOG_IF_FAILED(window->get_CurrentCanMinimize(&canDo));
    if (kind == ActionKind::windowMaximize)
        LOG_IF_FAILED(window->get_CurrentCanMaximize(&canDo));
    if (!canDo)
        return {true, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};

    PatternAttempt attempt{true, window->SetWindowVisualState(state)};
    LOG_IF_FAILED(attempt.hr);
    if (attempt.succeeded())
        method = "WindowPattern.SetWindowVisualState";
    return attempt;
}

// Turn a declined attempt into something a caller can act on.
std::string describe_decline(const PatternAttempt& attempt, const char* patternName) {
    if (!attempt.supported)
        return std::string("element does not support the ") + patternName + " pattern";
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(attempt.hr));
    return std::string("the ") + patternName + " pattern is present but the call failed (" +
           buf + ")";
}

} // namespace

bool parse_action_kind(const std::string& name, ActionKind& out) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "click")      { out = ActionKind::click;    return true; }
    if (lower == "invoke")     { out = ActionKind::invoke;   return true; }
    if (lower == "toggle")     { out = ActionKind::toggle;   return true; }
    if (lower == "setvalue" || lower == "set-value") { out = ActionKind::setValue; return true; }
    if (lower == "expand")     { out = ActionKind::expand;   return true; }
    if (lower == "collapse")   { out = ActionKind::collapse; return true; }
    if (lower == "select")     { out = ActionKind::select;   return true; }
    if (lower == "focus")      { out = ActionKind::focus;    return true; }
    if (lower == "scroll")     { out = ActionKind::scroll;   return true; }
    if (lower == "type" || lower == "typetext")  { out = ActionKind::typeText; return true; }
    if (lower == "key" || lower == "presskey")   { out = ActionKind::pressKey; return true; }
    return false;
}

const char* action_kind_name(ActionKind kind) {
    switch (kind) {
    case ActionKind::click:    return "click";
    case ActionKind::invoke:   return "invoke";
    case ActionKind::toggle:   return "toggle";
    case ActionKind::setValue: return "set-value";
    case ActionKind::expand:   return "expand";
    case ActionKind::collapse: return "collapse";
    case ActionKind::select:   return "select";
    case ActionKind::focus:    return "focus";
    case ActionKind::scroll:   return "scroll";
    case ActionKind::typeText: return "type";
    case ActionKind::pressKey: return "press-key";
    }
    return "unknown";
}

ActionResult perform_action(HWND hwnd, const UiaOptions& options,
                            const ActionRequest& request) {
    ActionResult result;

    // Waiting is not an action on a live element: it re-walks until the tree
    // says what the caller is waiting for, so it is handled before the
    // resolve-then-act path below.
    if (request.kind == ActionKind::waitFor || request.kind == ActionKind::waitGone) {
        if (request.elementRef.empty()) {
            result.message = "wait requires an element reference";
            return result;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds((std::max)(0, request.waitTimeoutMs));
        const bool wantPresent = request.kind == ActionKind::waitFor;

        for (;;) {
            UiaProvider provider;
            if (auto tree = provider.build(hwnd, options)) {
                assign_element_ids(*tree);
                assign_element_keys(*tree);
                const Element* found = find_element_by_ref(*tree, request.elementRef);

                bool satisfied = wantPresent ? found != nullptr : found == nullptr;
                // A property condition narrows "appeared" to "appeared and
                // settled", which is usually what a caller actually wants.
                if (satisfied && wantPresent && found && !request.waitProperty.empty()) {
                    const auto value = get_element_property(*found, request.waitProperty);
                    satisfied = value.has_value() && *value == request.waitValue;
                }
                if (satisfied) {
                    result.ok = true;
                    result.method = wantPresent ? "wait-for" : "wait-gone";
                    if (found) {
                        result.element = *found;
                        result.hasElement = true;
                    }
                    return result;
                }
            }

            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds((std::max)(10, request.pollIntervalMs)));
        }

        result.message = wantPresent
            ? "timed out waiting for '" + request.elementRef + "'" +
                  (request.waitProperty.empty()
                       ? std::string()
                       : " to report " + request.waitProperty + "=" + request.waitValue)
            : "timed out waiting for '" + request.elementRef + "' to disappear";
        return result;
    }

    const bool needsElement = !(request.kind == ActionKind::typeText ||
                                request.kind == ActionKind::pressKey) ||
                              !request.elementRef.empty();
    if (needsElement && request.elementRef.empty()) {
        result.message = std::string(action_kind_name(request.kind)) +
                         " requires an element reference";
        return result;
    }

    // Resolve the reference against a walk, then re-find the live element by
    // the RuntimeId that walk reported.
    Element target;
    std::vector<int> runtimeId;
    if (needsElement) {
        UiaProvider provider;
        auto tree = provider.build(hwnd, options);
        if (!tree) {
            result.message = "could not read the UI Automation tree for this window";
            return result;
        }
        lvt::assign_element_ids(*tree);
        lvt::assign_element_keys(*tree);

        const Element* found = find_element_by_ref(*tree, request.elementRef);
        if (!found) {
            result.message = "element '" + request.elementRef + "' not found";
            return result;
        }
        target = *found;

        const auto it = target.properties.find("RuntimeId");
        if (it == target.properties.end() || !parse_runtime_id(it->second, runtimeId)) {
            result.message = "element '" + request.elementRef +
                             "' has no usable RuntimeId to act on";
            return result;
        }
    }

    std::string method;
    std::string failure;
    bool realized = false;
    const POINT center = element_center(target);

    const HRESULT hr = run_on_mta([&]() -> HRESULT {
        wil::com_ptr<IUIAutomation> automation;
        RETURN_IF_FAILED(create_automation(options, &automation));

        wil::com_ptr<IUIAutomationElement> element;
        if (needsElement) {
            const HRESULT found = find_by_runtime_id(automation.get(), hwnd, runtimeId, &element);
            if (FAILED(found)) {
                failure = "element could not be located in the live tree; it may have "
                          "changed or disappeared since the walk";
                RETURN_HR(found);
            }
            // Virtualized items have to be made real before anything can touch
            // them, and this is a no-op on elements that are already real.
            if (realize_if_virtualized(element.get()))
                realized = true;
        }

        switch (request.kind) {
        case ActionKind::invoke: {
            const auto attempt = try_invoke(element.get(), method);
            if (attempt.succeeded())
                return S_OK;
            failure = describe_decline(attempt, "Invoke");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::click: {
            // Try the quiet routes first. A pattern that exists but fails is
            // just as much a reason to move on as one that is missing, which is
            // why both fall through rather than aborting.
            if (!request.forceSyntheticClick) {
                const auto invoked = try_invoke(element.get(), method);
                if (invoked.succeeded())
                    return S_OK;
                const auto legacy = try_legacy_default_action(element.get(), method);
                if (legacy.succeeded())
                    return S_OK;
            }

            // Nothing accepted it, so fall back to a real click. This is the
            // only path that needs the window on top and moves the cursor.
            if (target.bounds.width <= 0 || target.bounds.height <= 0) {
                failure = "no pattern would activate this element and it has no "
                          "on-screen bounds to click";
                return E_NOTIMPL;
            }
            if (!bring_to_foreground(hwnd)) {
                failure = "no pattern would activate this element, and the window "
                          "could not be brought to the foreground, so a synthetic "
                          "click would land on the wrong window";
                return E_ACCESSDENIED;
            }
            if (!send_click(center, request.button, request.amount)) {
                failure = "SendInput click failed";
                return E_FAIL;
            }
            method = "SendInput";
            return S_OK;
        }

        case ActionKind::toggle: {
            const auto attempt = try_toggle(element.get(), method);
            if (attempt.succeeded())
                return S_OK;
            failure = describe_decline(attempt, "Toggle");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::setValue: {
            const auto attempt = try_set_value(element.get(), request.text, method);
            if (attempt.succeeded())
                return S_OK;

            // Sliders and spinners carry their value through RangeValue rather
            // than Value, so a numeric argument gets a second chance there.
            const auto range = try_set_range_value(element.get(), request.text, method);
            if (range.succeeded())
                return S_OK;

            // No writable Value pattern: focus it and retype the contents.
            if (FAILED(element->SetFocus())) {
                failure = describe_decline(attempt, "Value") +
                          ", and the element could not be focused to type into";
                return E_NOTIMPL;
            }
            if (!bring_to_foreground(hwnd)) {
                failure = describe_decline(attempt, "Value") +
                          ", and the window could not be brought forward to type into it";
                return E_ACCESSDENIED;
            }
            KeyChord selectAll;
            if (parse_key_chord("Ctrl+A", selectAll))
                send_key_chord(selectAll);
            if (!send_text(request.text)) {
                failure = "SendInput typing failed";
                return E_FAIL;
            }
            method = "SetFocus+SendInput";
            return S_OK;
        }

        case ActionKind::expand:
        case ActionKind::collapse: {
            const bool expand = request.kind == ActionKind::expand;
            const auto attempt = try_expand_collapse(element.get(), expand, method);
            if (attempt.succeeded())
                return S_OK;
            failure = describe_decline(attempt, "ExpandCollapse");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::select: {
            const auto attempt = try_select(element.get(), method);
            if (attempt.succeeded())
                return S_OK;
            failure = describe_decline(attempt, "SelectionItem");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::addToSelection:
        case ActionKind::removeFromSelection: {
            const bool add = request.kind == ActionKind::addToSelection;
            const auto attempt = try_change_selection(element.get(), add, method);
            if (attempt.succeeded())
                return S_OK;
            failure = describe_decline(attempt, "SelectionItem");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::selectText: {
            const auto attempt = try_select_text(element.get(), request.text, method);
            if (attempt.succeeded())
                return S_OK;
            if (attempt.supported && attempt.hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
                failure = "text '" + request.text + "' was not found in this element";
                return attempt.hr;
            }
            failure = describe_decline(attempt, "Text");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::windowClose:
        case ActionKind::windowMinimize:
        case ActionKind::windowMaximize:
        case ActionKind::windowRestore: {
            const auto attempt = try_window_action(element.get(), request.kind, method);
            if (attempt.succeeded())
                return S_OK;
            if (attempt.supported && attempt.hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
                failure = "this window does not allow that state change";
                return attempt.hr;
            }
            failure = describe_decline(attempt, "Window");
            return attempt.supported ? attempt.hr : E_NOTIMPL;
        }

        case ActionKind::focus: {
            const HRESULT hr = element->SetFocus();
            if (FAILED(hr)) {
                failure = "the element refused focus";
                RETURN_HR(hr);
            }
            method = "SetFocus";
            return S_OK;
        }

        case ActionKind::scroll: {
            const auto attempt = try_scroll(element.get(), request.direction,
                                            request.amount, method);
            if (attempt.succeeded())
                return S_OK;

            if (!bring_to_foreground(hwnd)) {
                failure = describe_decline(attempt, "Scroll") +
                          ", and the window could not be brought forward to send a "
                          "wheel event";
                return E_ACCESSDENIED;
            }
            const int notch = WHEEL_DELTA * (std::max)(1, request.amount);
            const bool horizontal = request.direction == "left" || request.direction == "right";
            const bool negative = request.direction == "down" || request.direction == "left";
            if (!send_wheel(center, negative ? -notch : notch, horizontal)) {
                failure = "SendInput wheel failed";
                return E_FAIL;
            }
            method = "SendInput";
            return S_OK;
        }

        case ActionKind::typeText:
        case ActionKind::pressKey: {
            // Synthetic keyboard input goes to the focused element of the
            // foreground window, so both prerequisites have to hold.
            if (element && FAILED(element->SetFocus())) {
                failure = "could not focus the element to send input to it";
                return E_NOTIMPL;
            }
            if (!bring_to_foreground(hwnd)) {
                failure = "could not bring the target window to the foreground, so "
                          "keyboard input would go elsewhere";
                return E_ACCESSDENIED;
            }
            if (request.kind == ActionKind::typeText) {
                if (!send_text(request.text)) {
                    failure = "SendInput typing failed";
                    return E_FAIL;
                }
                method = element ? "SetFocus+SendInput" : "SendInput";
                return S_OK;
            }

            std::vector<KeyChord> chords;
            if (!parse_key_chords(request.text, chords)) {
                failure = "could not parse key chord '" + request.text + "'";
                return E_INVALIDARG;
            }
            for (const auto& chord : chords) {
                if (!send_key_chord(chord)) {
                    failure = "SendInput key chord failed";
                    return E_FAIL;
                }
            }
            method = element ? "SetFocus+SendInput" : "SendInput";
            return S_OK;
        }
        }
        return E_NOTIMPL;
    });

    if (FAILED(hr)) {
        result.message = failure.empty() ? "action failed" : failure;
        return result;
    }

    result.ok = true;
    result.method = realized ? "VirtualizedItem.Realize+" + method : method;

    // Report the element as it is *after* the action, so a caller can see the
    // effect without a second walk. Re-found by RuntimeId because ids shift
    // whenever the tree changes, which an action may well have caused.
    if (needsElement) {
        UiaProvider provider;
        if (auto after = provider.build(hwnd, options)) {
            lvt::assign_element_ids(*after);
            lvt::assign_element_keys(*after);
            const auto it = target.properties.find("RuntimeId");
            if (it != target.properties.end()) {
                if (const Element* refreshed = find_element_by_ref(*after, "uia:" + it->second)) {
                    result.element = *refreshed;
                    result.hasElement = true;
                }
            }
        }
    }
    return result;
}

} // namespace lvt
