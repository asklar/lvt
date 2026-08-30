#include "uia_provider.h"
#include "uia_actions.h"
#include "native_win_event.h"
#include "uia_property_adapter.h"
#include "../debug.h"

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <oleacc.h>
#include <UIAutomation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lvt {

class UiaWindowLifetimeToken {
public:
    static std::shared_ptr<UiaWindowLifetimeToken> create(
        HWND hwnd, DWORD pid,
        uint64_t processCreationIdentity) {
        auto token = std::shared_ptr<UiaWindowLifetimeToken>(
            new (std::nothrow) UiaWindowLifetimeToken(
                hwnd, pid, processCreationIdentity));
        if (!token || !token->initialize())
            return {};
        return token;
    }

    ~UiaWindowLifetimeToken() {
        if (!m_propertyInstalled)
            return;
        if (exact_owner_and_process() &&
            GetPropW(m_hwnd, m_propertyName.c_str()) ==
                m_propertyValue) {
            RemovePropW(m_hwnd, m_propertyName.c_str());
        }
    }

    bool matches() {
        if (m_testInvalidation &&
            WaitForSingleObject(
                m_testInvalidation.get(), 0) ==
                WAIT_OBJECT_0) {
            return false;
        }
        if (!exact_owner_and_process())
            return false;
        if (m_propertyInstalled) {
            return GetPropW(
                       m_hwnd, m_propertyName.c_str()) ==
                   m_propertyValue;
        }
        if (!m_destroyWatcher ||
            !m_destroyWatcher->synchronize()) {
            return false;
        }
        return !m_destroyWatcher->root_destroyed() &&
               !m_destroyWatcher->target_exited();
    }

private:
    UiaWindowLifetimeToken(
        HWND hwnd, DWORD pid,
        uint64_t processCreationIdentity)
        : m_hwnd(hwnd), m_pid(pid),
          m_processCreationIdentity(
              processCreationIdentity),
          m_propertyValue(
              reinterpret_cast<HANDLE>(this)) {
    }

    bool exact_owner_and_process() const {
        DWORD currentPid = 0;
        return m_hwnd && IsWindow(m_hwnd) &&
               GetWindowThreadProcessId(
                   m_hwnd, &currentPid) != 0 &&
               currentPid == m_pid &&
               process_creation_identity(m_pid) ==
                   m_processCreationIdentity;
    }

    bool initialize() {
        if (!exact_owner_and_process())
            return false;

        char invalidationEvent[160]{};
        const DWORD invalidationLength =
            GetEnvironmentVariableA(
                "LVT_TEST_UIA_LIFETIME_INVALIDATION_EVENT",
                invalidationEvent,
                static_cast<DWORD>(
                    _countof(invalidationEvent)));
        if (invalidationLength > 0 &&
            invalidationLength < _countof(invalidationEvent)) {
            m_testInvalidation.reset(OpenEventA(
                SYNCHRONIZE, FALSE, invalidationEvent));
            if (!m_testInvalidation)
                return false;
        }

        GUID guid{};
        if (SUCCEEDED(CoCreateGuid(&guid))) {
            wchar_t guidText[64]{};
            if (StringFromGUID2(
                    guid, guidText,
                    static_cast<int>(_countof(guidText))) > 0) {
                m_propertyName =
                    L"lvt.uia.window." +
                    std::wstring(guidText);
            }
        }
        if (m_propertyName.empty())
            return false;

        char forceFailure[8]{};
        const bool forcePropertyFailure =
            GetEnvironmentVariableA(
                "LVT_TEST_UIA_FORCE_WINDOW_PROP_FAILURE",
                forceFailure,
                static_cast<DWORD>(
                    _countof(forceFailure))) != 0 &&
            strtoul(forceFailure, nullptr, 10) != 0;

        SetLastError(ERROR_SUCCESS);
        if (!forcePropertyFailure &&
            SetPropW(
                m_hwnd, m_propertyName.c_str(),
                m_propertyValue) &&
            GetPropW(m_hwnd, m_propertyName.c_str()) ==
                m_propertyValue) {
            m_propertyInstalled = true;
            return true;
        }

        const DWORD propertyError = GetLastError();
        if (GetPropW(m_hwnd, m_propertyName.c_str()) ==
            m_propertyValue) {
            RemovePropW(m_hwnd, m_propertyName.c_str());
        }
        m_destroyWatcher =
            native_eventing_detail::NativeWinEventSource::create(
                m_hwnd, m_pid);
        if (m_destroyWatcher &&
            m_destroyWatcher->hook_active()) {
            return true;
        }
        if (g_debug) {
            fprintf(
                stderr,
                "lvt: UIA window lifetime sentinel failed "
                "(SetProp error %lu, WinEvent unavailable)\n",
                static_cast<unsigned long>(propertyError));
        }
        m_destroyWatcher.reset();
        return false;
    }

    HWND m_hwnd = nullptr;
    DWORD m_pid = 0;
    uint64_t m_processCreationIdentity = 0;
    std::wstring m_propertyName;
    HANDLE m_propertyValue = nullptr;
    bool m_propertyInstalled = false;
    std::unique_ptr<
        native_eventing_detail::NativeWinEventSource>
        m_destroyWatcher;
    wil::unique_handle m_testInvalidation;
};

namespace {

using clock_type = std::chrono::steady_clock;
static constexpr DWORD kUiaDefaultTransactionTimeoutMs = 20000;
static constexpr DWORD kUiaConnectionTimeoutCapMs = 2000;
static constexpr auto kUiaTargetLivenessInterval = std::chrono::milliseconds(100);

std::atomic_uint64_t g_uiaEventConnections = 0;
std::atomic_uint64_t g_uiaStructureRegistrations = 0;
std::atomic_uint64_t g_uiaPropertyRegistrations = 0;
std::atomic_uint64_t g_uiaAutomationRegistrations = 0;
std::atomic_uint64_t g_uiaEventCallbacks = 0;
std::atomic_uint64_t g_uiaRemoveAllCalls = 0;

const std::vector<int>& uia_event_property_ids() {
    static const std::vector<int> ids = {
        UIA_NamePropertyId,
        UIA_AutomationIdPropertyId,
        UIA_ClassNamePropertyId,
        UIA_ControlTypePropertyId,
        UIA_FrameworkIdPropertyId,
        UIA_BoundingRectanglePropertyId,
        UIA_IsOffscreenPropertyId,
        UIA_IsEnabledPropertyId,
        UIA_IsControlElementPropertyId,
        UIA_IsContentElementPropertyId,
        UIA_HasKeyboardFocusPropertyId,
        UIA_IsKeyboardFocusablePropertyId,

        UIA_IsValuePatternAvailablePropertyId,
        UIA_ValueValuePropertyId,
        UIA_ValueIsReadOnlyPropertyId,

        UIA_IsRangeValuePatternAvailablePropertyId,
        UIA_RangeValueValuePropertyId,
        UIA_RangeValueMinimumPropertyId,
        UIA_RangeValueMaximumPropertyId,
        UIA_RangeValueSmallChangePropertyId,
        UIA_RangeValueLargeChangePropertyId,
        UIA_RangeValueIsReadOnlyPropertyId,

        UIA_IsTogglePatternAvailablePropertyId,
        UIA_ToggleToggleStatePropertyId,

        UIA_IsExpandCollapsePatternAvailablePropertyId,
        UIA_ExpandCollapseExpandCollapseStatePropertyId,

        UIA_IsSelectionPatternAvailablePropertyId,
        UIA_SelectionCanSelectMultiplePropertyId,
        UIA_SelectionIsSelectionRequiredPropertyId,
        UIA_IsSelectionItemPatternAvailablePropertyId,
        UIA_SelectionItemIsSelectedPropertyId,

        UIA_IsScrollPatternAvailablePropertyId,
        UIA_ScrollHorizontalScrollPercentPropertyId,
        UIA_ScrollVerticalScrollPercentPropertyId,
        UIA_ScrollHorizontalViewSizePropertyId,
        UIA_ScrollVerticalViewSizePropertyId,
        UIA_ScrollHorizontallyScrollablePropertyId,
        UIA_ScrollVerticallyScrollablePropertyId,
    };
    return ids;
}

const std::vector<int>& uia_automation_event_ids() {
    static const std::vector<int> ids = {
        UIA_Invoke_InvokedEventId,
        UIA_SelectionItem_ElementAddedToSelectionEventId,
        UIA_SelectionItem_ElementRemovedFromSelectionEventId,
        UIA_SelectionItem_ElementSelectedEventId,
        UIA_Text_TextChangedEventId,
        UIA_Text_TextSelectionChangedEventId,
        UIA_MenuOpenedEventId,
        UIA_MenuClosedEventId,
        UIA_LayoutInvalidatedEventId,
    };
    return ids;
}

bool exact_window_matches(HWND hwnd, DWORD pid) {
    if (!hwnd || !IsWindow(hwnd))
        return false;
    DWORD currentPid = 0;
    GetWindowThreadProcessId(hwnd, &currentPid);
    return currentPid != 0 && currentPid == pid;
}

bool is_uia_ownership_failure(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_INVALID_OWNER) ||
           hr == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
}

bool target_process_matches(
    const UiaTargetIdentity& identity, HANDLE retainedProcess) {
    if (retainedProcess) {
        return process_identity_matches(
            retainedProcess, identity.pid,
            identity.processCreationIdentity);
    }
    wil::unique_process_handle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, identity.pid));
    return process_identity_matches(
        process.get(), identity.pid,
        identity.processCreationIdentity);
}

bool target_identity_matches(
    const UiaTargetIdentity& identity,
    HANDLE retainedProcess) {
    return identity.valid() &&
           exact_window_matches(
               identity.hwnd, identity.pid) &&
           target_process_matches(
               identity, retainedProcess) &&
           identity.windowLifetime->matches();
}

bool read_runtime_id(
    IUIAutomationElement* element,
    std::vector<int>& runtimeId) {
    runtimeId.clear();
    if (!element)
        return false;
    wil::unique_variant value;
    if (FAILED(element->GetCurrentPropertyValue(
            UIA_RuntimeIdPropertyId, &value)) ||
        value.vt != (VT_ARRAY | VT_I4) ||
        !value.parray ||
        SafeArrayGetDim(value.parray) != 1) {
        return false;
    }
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(
            value.parray, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(
            value.parray, 1, &upper)) ||
        upper < lower) {
        return false;
    }
    runtimeId.reserve(
        static_cast<size_t>(upper - lower + 1));
    for (LONG index = lower; index <= upper; ++index) {
        int component = 0;
        if (FAILED(SafeArrayGetElement(
                value.parray, &index, &component))) {
            runtimeId.clear();
            return false;
        }
        runtimeId.push_back(component);
    }
    return !runtimeId.empty();
}

void wait_after_element_from_handle_test_gate(
    const char* environmentName) {
    if (!environmentName)
        return;
    char base[160]{};
    const DWORD length = GetEnvironmentVariableA(
        environmentName, base,
        static_cast<DWORD>(_countof(base)));
    if (length == 0 || length >= _countof(base))
        return;

    char enteredName[192]{};
    char releaseName[192]{};
    snprintf(
        enteredName, sizeof(enteredName), "%s-entered", base);
    snprintf(
        releaseName, sizeof(releaseName), "%s-release", base);
    wil::unique_handle entered(OpenEventA(
        EVENT_MODIFY_STATE, FALSE, enteredName));
    wil::unique_handle release(OpenEventA(
        SYNCHRONIZE, FALSE, releaseName));
    if (!entered || !release)
        return;
    SetEvent(entered.get());
    WaitForSingleObject(release.get(), 10000);
}

void append_uia_event_test_stat(
    const char* operation, HWND hwnd, DWORD pid,
    bool snapshotRequired = false) {
    char path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "LVT_TEST_UIA_EVENT_STATS", path,
        static_cast<DWORD>(_countof(path)));
    if (length == 0 || length >= _countof(path))
        return;

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    wil::unique_handle file(CreateFileA(
        path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return;

    char line[160]{};
    const int written = snprintf(
        line, sizeof(line), "%s hwnd=0x%llX pid=%lu snapshot=%d\r\n",
        operation,
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(hwnd)),
        static_cast<unsigned long>(pid),
        snapshotRequired ? 1 : 0);
    if (written <= 0)
        return;
    DWORD ignored = 0;
    WriteFile(
        file.get(), line,
        static_cast<DWORD>(
            (std::min)(written, static_cast<int>(sizeof(line) - 1))),
        &ignored, nullptr);
}

bool fail_connected_tree_for_testing() {
    char value[16]{};
    if (GetEnvironmentVariableA(
            "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE",
            value, static_cast<DWORD>(_countof(value))) == 0 ||
        strtoul(value, nullptr, 10) == 0) {
        return false;
    }
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (GetEnvironmentVariableA(
            "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE",
            value, static_cast<DWORD>(_countof(value))) == 0 ||
        strtoul(value, nullptr, 10) == 0) {
        return false;
    }
    SetEnvironmentVariableA(
        "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE", "0");
    return true;
}

class UiaEventHandler final
    : public IUIAutomationStructureChangedEventHandler,
      public IUIAutomationPropertyChangedEventHandler,
      public IUIAutomationEventHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) noexcept override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) ||
            iid == __uuidof(IUIAutomationStructureChangedEventHandler)) {
            *object =
                static_cast<IUIAutomationStructureChangedEventHandler*>(this);
        } else if (iid ==
                   __uuidof(IUIAutomationPropertyChangedEventHandler)) {
            *object =
                static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        } else if (iid == __uuidof(IUIAutomationEventHandler)) {
            *object = static_cast<IUIAutomationEventHandler*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining =
            m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(
        IUIAutomationElement*, StructureChangeType, SAFEARRAY*) noexcept override {
        signal();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(
        IUIAutomationElement*, PROPERTYID, VARIANT) noexcept override {
        signal();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(
        IUIAutomationElement*, EVENTID) noexcept override {
        signal();
        return S_OK;
    }

    bool consume() noexcept {
        return m_hint.consume();
    }

    void stop() noexcept {
        m_hint.stop();
    }

private:
    void signal() noexcept {
        g_uiaEventCallbacks.fetch_add(1, std::memory_order_relaxed);
        m_hint.signal();
    }

    std::atomic<ULONG> m_references{1};
    uia_eventing_detail::SnapshotHint m_hint;
};

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

std::string format_double_round_trip(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*g",
             std::numeric_limits<double>::max_digits10, value);
    return buf;
}

std::string format_float_round_trip(float value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*g",
             std::numeric_limits<float>::max_digits10, value);
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
            out += format_double_round_trip(element);
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
    case VT_R4:   return format_float_round_trip(v.fltVal);
    case VT_R8:   return format_double_round_trip(v.dblVal);
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

std::string identity_scope(const UiaOptions& options) {
    std::vector<std::string> properties = options.extraProperties;
    std::sort(properties.begin(), properties.end());
    std::string scope =
        std::string(uia_view_name(options.view)) +
        "|timeout=" + std::to_string(options.timeoutMs);
    for (const auto& property : properties)
        scope += "|property=" + property;
    return scope;
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

struct UiaEventRegistration {
    wil::com_ptr<IUIAutomationElement> root;
    wil::com_ptr<IUIAutomationCacheRequest> cacheRequest;
    wil::com_ptr<UiaEventHandler> handler;
};

HRESULT register_uia_event_handlers(
    IUIAutomation* automation,
    const UiaTargetIdentity& identity,
    HANDLE retainedProcess,
    UiaEventRegistration& registration) {
    RETURN_HR_IF_NULL(E_POINTER, automation);

    wil::com_ptr<IUIAutomationElement> root;
    RETURN_IF_FAILED(get_validated_uia_root(
        automation, identity, retainedProcess, &root));

    wil::com_ptr<IUIAutomationCacheRequest> cacheRequest;
    RETURN_IF_FAILED(automation->CreateCacheRequest(&cacheRequest));
    RETURN_IF_FAILED(cacheRequest->put_TreeScope(TreeScope_Element));
    RETURN_IF_FAILED(
        cacheRequest->put_AutomationElementMode(AutomationElementMode_None));
    RETURN_IF_FAILED(cacheRequest->AddProperty(UIA_RuntimeIdPropertyId));
    RETURN_IF_FAILED(cacheRequest->AddProperty(UIA_ProcessIdPropertyId));
    RETURN_IF_FAILED(cacheRequest->AddProperty(UIA_NativeWindowHandlePropertyId));

    wil::com_ptr<UiaEventHandler> handler;
    handler.attach(new (std::nothrow) UiaEventHandler());
    RETURN_IF_NULL_ALLOC(handler);

    const HRESULT structureHr =
        automation->AddStructureChangedEventHandler(
            root.get(), TreeScope_Subtree, cacheRequest.get(),
            static_cast<IUIAutomationStructureChangedEventHandler*>(
                handler.get()));
    if (FAILED(structureHr))
        RETURN_HR(structureHr);
    g_uiaStructureRegistrations.fetch_add(1, std::memory_order_relaxed);

    const auto& properties = uia_event_property_ids();
    const HRESULT propertyHr =
        automation->AddPropertyChangedEventHandlerNativeArray(
            root.get(), TreeScope_Subtree, cacheRequest.get(),
            static_cast<IUIAutomationPropertyChangedEventHandler*>(
                handler.get()),
            const_cast<PROPERTYID*>(properties.data()),
            static_cast<int>(properties.size()));
    if (FAILED(propertyHr)) {
        (void)automation->RemoveAllEventHandlers();
        RETURN_HR(propertyHr);
    }
    g_uiaPropertyRegistrations.fetch_add(1, std::memory_order_relaxed);

    uint64_t automationRegistrations = 0;
    for (const EVENTID eventId : uia_automation_event_ids()) {
        const HRESULT eventHr = automation->AddAutomationEventHandler(
            eventId, root.get(), TreeScope_Subtree, cacheRequest.get(),
            static_cast<IUIAutomationEventHandler*>(handler.get()));
        if (SUCCEEDED(eventHr)) {
            ++automationRegistrations;
        } else {
            LOG_IF_FAILED(eventHr);
        }
    }
    if (automationRegistrations == 0) {
        (void)automation->RemoveAllEventHandlers();
        RETURN_HR(E_FAIL);
    }
    g_uiaAutomationRegistrations.fetch_add(
        automationRegistrations, std::memory_order_relaxed);
    g_uiaEventConnections.fetch_add(1, std::memory_order_relaxed);

    registration.root = std::move(root);
    registration.cacheRequest = std::move(cacheRequest);
    registration.handler = std::move(handler);
    return S_OK;
}

HRESULT build_tree_with_automation(IUIAutomation* automation,
                                   const UiaTargetIdentity& identity,
                                   HANDLE retainedProcess,
                                   const char* testGateEnvironment,
                                   const UiaOptions& options,
                                   Element& out, bool& truncated) {
    wil::com_ptr<IUIAutomationElement> root;
    RETURN_IF_FAILED(get_validated_uia_root(
        automation, identity, retainedProcess, &root,
        testGateEnvironment));

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

HRESULT build_tree_on_mta(
                          const UiaTargetIdentity& identity,
                          const UiaOptions& options,
                          Element& out, bool& truncated) {
    wil::com_ptr<IUIAutomation> automation;
    RETURN_IF_FAILED(create_automation(options, &automation));
    return build_tree_with_automation(
        automation.get(), identity, nullptr,
        "LVT_TEST_UIA_ONE_SHOT_AFTER_ELEMENT_GATE",
        options, out, truncated);
}

// UIA clients belong in an MTA. screenshot.cpp initializes an STA on the calling
// thread, and a thread cannot be in both, so all UIA work is marshalled onto a
// dedicated MTA thread.
//
// One-shot callers create and release their COM objects inside `fn`.
// UiaConnection instead owns one long-lived MTA worker (see State below), so
// its client, handlers, tree reads, event drains, and teardown all stay on one
// exact thread.
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

HRESULT get_validated_uia_root(
    IUIAutomation* automation,
    const UiaTargetIdentity& identity,
    HANDLE retainedProcess,
    IUIAutomationElement** root,
    const char* testGateEnvironment) {
    RETURN_HR_IF_NULL(E_POINTER, automation);
    RETURN_HR_IF_NULL(E_POINTER, root);
    *root = nullptr;
    RETURN_HR_IF(E_INVALIDARG, !identity.valid());
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
        !target_identity_matches(
            identity, retainedProcess));

    wil::com_ptr<IUIAutomationElement> candidate;
    const HRESULT elementHr = automation->ElementFromHandle(
        identity.hwnd, &candidate);
    if (FAILED(elementHr)) {
        RETURN_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
            !target_identity_matches(
                identity, retainedProcess));
        RETURN_HR(elementHr);
    }
    RETURN_HR_IF_NULL(E_FAIL, candidate.get());

    // This is deliberately the first work after ElementFromHandle. A window
    // can be destroyed and its numeric HWND recycled between the caller's
    // precheck and this cross-process acquisition.
    wait_after_element_from_handle_test_gate(
        testGateEnvironment);
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
        !target_identity_matches(
            identity, retainedProcess));

    int rootPid = 0;
    RETURN_IF_FAILED(candidate->get_CurrentProcessId(&rootPid));
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
        rootPid <= 0 ||
        static_cast<DWORD>(rootPid) != identity.pid);

    std::vector<int> runtimeId;
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
        !read_runtime_id(candidate.get(), runtimeId) ||
        runtimeId != identity.rootRuntimeId);

    *root = candidate.detach();
    return S_OK;
}

std::optional<UiaTargetIdentity> capture_uia_target_identity(
    HWND hwnd, DWORD expectedPid,
    uint64_t expectedProcessCreationIdentity) {
    if (!expectedPid || !exact_window_matches(hwnd, expectedPid))
        return std::nullopt;

    wil::unique_process_handle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, expectedPid));
    if (!process)
        return std::nullopt;
    const uint64_t creationIdentity =
        expectedProcessCreationIdentity
            ? expectedProcessCreationIdentity
            : process_creation_identity(process.get());
    if (!process_identity_matches(
            process.get(), expectedPid, creationIdentity)) {
        return std::nullopt;
    }

    UiaTargetIdentity identity;
    identity.hwnd = hwnd;
    identity.pid = expectedPid;
    identity.processCreationIdentity = creationIdentity;
    identity.windowLifetime =
        UiaWindowLifetimeToken::create(
            hwnd, expectedPid, creationIdentity);
    if (!identity.windowLifetime)
        return std::nullopt;
    const HRESULT hr = run_on_mta([&]() -> HRESULT {
        wil::com_ptr<IUIAutomation> automation;
        UiaOptions options;
        options.timeoutMs = 0;
        RETURN_IF_FAILED(create_automation(options, &automation));

        wil::com_ptr<IUIAutomationElement> root;
        RETURN_IF_FAILED(automation->ElementFromHandle(
            identity.hwnd, &root));
        RETURN_HR_IF_NULL(E_FAIL, root.get());
        RETURN_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
            !exact_window_matches(
                identity.hwnd, identity.pid) ||
            !target_process_matches(
                identity, process.get()) ||
            !identity.windowLifetime->matches());

        int rootPid = 0;
        RETURN_IF_FAILED(root->get_CurrentProcessId(&rootPid));
        RETURN_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
            rootPid <= 0 ||
            static_cast<DWORD>(rootPid) != identity.pid);
        RETURN_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
            !read_runtime_id(root.get(), identity.rootRuntimeId));
        return S_OK;
    });
    if (FAILED(hr) || !identity.valid())
        return std::nullopt;
    return identity;
}

namespace uia_eventing_detail {

SubscriptionCounters subscription_counters() {
    return {
        .connections =
            g_uiaEventConnections.load(std::memory_order_relaxed),
        .structureRegistrations =
            g_uiaStructureRegistrations.load(std::memory_order_relaxed),
        .propertyRegistrations =
            g_uiaPropertyRegistrations.load(std::memory_order_relaxed),
        .automationRegistrations =
            g_uiaAutomationRegistrations.load(std::memory_order_relaxed),
        .callbacks =
            g_uiaEventCallbacks.load(std::memory_order_relaxed),
        .removeAllCalls =
            g_uiaRemoveAllCalls.load(std::memory_order_relaxed),
    };
}

void reset_subscription_counters() {
    g_uiaEventConnections.store(0, std::memory_order_relaxed);
    g_uiaStructureRegistrations.store(0, std::memory_order_relaxed);
    g_uiaPropertyRegistrations.store(0, std::memory_order_relaxed);
    g_uiaAutomationRegistrations.store(0, std::memory_order_relaxed);
    g_uiaEventCallbacks.store(0, std::memory_order_relaxed);
    g_uiaRemoveAllCalls.store(0, std::memory_order_relaxed);
}

const std::vector<int>& subscribed_property_ids() {
    return uia_event_property_ids();
}

const std::vector<int>& subscribed_automation_event_ids() {
    return uia_automation_event_ids();
}

} // namespace uia_eventing_detail

std::string format_uia_double(double value) {
    return format_double_round_trip(value);
}

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

std::optional<Element> UiaProvider::build(
    const UiaTargetIdentity& identity,
    const UiaOptions& options,
    bool* truncated,
    bool* ownershipLost) {
    Element root;
    bool wasTruncated = false;
    if (ownershipLost)
        *ownershipLost = false;

    const HRESULT hr = run_on_mta([&] {
        return build_tree_on_mta(
            identity, options, root, wasTruncated);
    });

    if (truncated)
        *truncated = wasTruncated;

    if (FAILED(hr)) {
        if (ownershipLost) {
            *ownershipLost =
                hr == HRESULT_FROM_WIN32(ERROR_INVALID_OWNER) ||
                hr == HRESULT_FROM_WIN32(
                    ERROR_INVALID_WINDOW_HANDLE);
        }
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
    explicit State(UiaTargetIdentity target)
        : targetIdentity(std::move(target)),
          worker([this] { worker_main(); }) {
        std::unique_lock<std::mutex> lock(queueMutex);
        readyCondition.wait(lock, [this] { return ready; });
    }

    ~State() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopping = true;
        }
        queueCondition.notify_one();
        if (worker.joinable())
            worker.join();
    }

    HRESULT invoke(std::function<HRESULT()> operation) {
        auto completion = std::make_shared<std::promise<HRESULT>>();
        auto future = completion->get_future();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopping)
                return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
            if (FAILED(initializeResult))
                return initializeResult;
            operations.emplace_back(
                [operation = std::move(operation),
                 completion = std::move(completion)]() mutable {
                    const HRESULT result = [&]() -> HRESULT {
                        try {
                            return operation();
                        }
                        CATCH_RETURN();
                    }();
                    completion->set_value(result);
                });
        }
        queueCondition.notify_one();
        return future.get();
    }

    bool connected() const noexcept {
        return alive.load(std::memory_order_acquire);
    }

    std::mutex operationMutex;
    std::atomic<bool> failNextTree{false};
    UiaPropertyIdentityCache identities;
    UiaPropertySchemaCache schemaCache;
    std::string identityError;

private:
    HRESULT initialize_on_mta() {
        process.reset(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, targetIdentity.pid));
        RETURN_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_OWNER),
            !process ||
            !target_identity_matches(
                targetIdentity, process.get()));
        UiaOptions connectOptions;
        connectOptions.timeoutMs = 0;
        RETURN_IF_FAILED(create_automation(connectOptions, &automation));
        const HRESULT eventHr = register_uia_event_handlers(
            automation.get(), targetIdentity, process.get(),
            eventRegistration);
        if (FAILED(eventHr)) {
            automation.reset();
            RETURN_HR(eventHr);
        }
        alive.store(true, std::memory_order_release);
        return S_OK;
    }

    void teardown_on_mta() noexcept {
        if (eventRegistration.handler)
            eventRegistration.handler->stop();
        if (automation && eventRegistration.handler) {
            g_uiaRemoveAllCalls.fetch_add(1, std::memory_order_relaxed);
            LOG_IF_FAILED(automation->RemoveAllEventHandlers());
        }
        eventRegistration.handler.reset();
        eventRegistration.cacheRequest.reset();
        eventRegistration.root.reset();
        automation.reset();
        alive.store(false, std::memory_order_release);
    }

    void worker_main() {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT result = coHr;
        if (SUCCEEDED(coHr)) {
            result = [&]() -> HRESULT {
                try {
                    return initialize_on_mta();
                }
                CATCH_RETURN();
            }();
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            initializeResult = result;
            ready = true;
        }
        readyCondition.notify_one();

        if (SUCCEEDED(coHr)) {
            for (;;) {
                std::function<void()> operation;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    queueCondition.wait_for(
                        lock, kUiaTargetLivenessInterval,
                        [this] {
                            return stopping || !operations.empty();
                        });
                    if (stopping && operations.empty())
                        break;
                    if (!operations.empty()) {
                        operation = std::move(operations.front());
                        operations.pop_front();
                    }
                }

                if (operation)
                    operation();

                if (connected() &&
                    !target_identity_matches(
                        targetIdentity, process.get())) {
                    teardown_on_mta();
                }
            }
            teardown_on_mta();
            CoUninitialize();
        }
    }

    UiaTargetIdentity targetIdentity;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::condition_variable readyCondition;
    std::deque<std::function<void()>> operations;
    bool ready = false;
    bool stopping = false;
    HRESULT initializeResult = E_PENDING;
    std::thread worker;
    std::atomic<bool> alive{false};
    wil::unique_process_handle process;
    wil::com_ptr<IUIAutomation> automation;
    UiaEventRegistration eventRegistration;

    friend class UiaConnection;
};

bool UiaPropertyIdentityCache::attach(
    Element& root, const std::string& scope, bool completeSnapshot) {
    if (!m_scopes.contains(scope) &&
        m_scopes.size() >= kMaximumScopes) {
        return false;
    }
    std::unordered_set<std::string> snapshotRuntimeIds;
    collect_runtime_ids(root, snapshotRuntimeIds);
    std::unordered_set<std::string> protectedRuntimeIds;
    for (const auto& [knownScope, current] :
         m_currentRuntimeIdsByScope) {
        if (completeSnapshot && knownScope == scope)
            continue;
        protectedRuntimeIds.insert(current.begin(), current.end());
    }
    protectedRuntimeIds.insert(
        snapshotRuntimeIds.begin(), snapshotRuntimeIds.end());
    if (protectedRuntimeIds.size() > kMaximumRuntimeIds)
        return false;
    if (completeSnapshot)
        m_currentRuntimeIdsByScope[scope] = snapshotRuntimeIds;

    auto& scopeState = m_scopes[scope];
    scopeState.lastUsed = ++m_clock;
    if (completeSnapshot)
        ++scopeState.generation;
    else if (scopeState.generation == 0)
        scopeState.generation = 1;
    m_activeScope = scope;
    m_activeGeneration = scopeState.generation;
    attach_element(root);
    prune(protectedRuntimeIds);
    return true;
}

void UiaPropertyIdentityCache::attach_element(Element& element) {
    const auto runtime = element.properties.find("RuntimeId");
    if (runtime != element.properties.end() && !runtime->second.empty()) {
        auto found = m_handlesByRuntimeId.find(runtime->second);
        if (found != m_handlesByRuntimeId.end()) {
            found->second.lastUsed = ++m_clock;
            found->second.lastSeenByScope[m_activeScope] =
                m_activeGeneration;
            element.providerHandle = found->second.handle;
        } else {
            const auto handle = m_nextHandle++;
            m_handlesByRuntimeId.emplace(
                runtime->second,
                RuntimeIdentity{
                    .handle = handle,
                    .lastUsed = ++m_clock,
                    .lastSeenByScope = {
                        {m_activeScope, m_activeGeneration},
                    },
                });
            m_runtimeIdsByHandle.emplace(handle, runtime->second);
            element.providerHandle = handle;
        }
    }
    for (auto& child : element.children)
        attach_element(child);
}

bool UiaPropertyIdentityCache::remember(
    const Element& root, const std::string& scope,
    bool completeSnapshot) {
    if (!m_scopes.contains(scope) &&
        m_scopes.size() >= kMaximumScopes) {
        return false;
    }
    std::unordered_set<std::string> snapshotKeys;
    collect_keys(root, snapshotKeys);
    std::unordered_set<std::string> protectedKeys;
    for (const auto& [knownScope, current] : m_currentKeysByScope) {
        if (completeSnapshot && knownScope == scope)
            continue;
        protectedKeys.insert(current.begin(), current.end());
    }
    protectedKeys.insert(snapshotKeys.begin(), snapshotKeys.end());
    if (protectedKeys.size() > kMaximumKeyAliases)
        return false;
    if (completeSnapshot)
        m_currentKeysByScope[scope] = snapshotKeys;

    auto& scopeState = m_scopes[scope];
    scopeState.lastUsed = ++m_clock;
    if (scopeState.generation == 0)
        scopeState.generation = 1;
    m_activeScope = scope;
    m_activeGeneration = scopeState.generation;
    remember_element(root);
    std::unordered_set<std::string> protectedRuntimeIds;
    for (const auto& [_, current] : m_currentRuntimeIdsByScope)
        protectedRuntimeIds.insert(current.begin(), current.end());
    prune(protectedRuntimeIds, protectedKeys);
    return true;
}

void UiaPropertyIdentityCache::remember_element(const Element& element) {
    const auto runtime = element.properties.find("RuntimeId");
    if (!element.key.empty() &&
        runtime != element.properties.end() && !runtime->second.empty()) {
        m_runtimeIdsByKey[element.key][runtime->second] = ++m_clock;
    }
    for (const auto& child : element.children)
        remember_element(child);
}

std::optional<uint64_t> UiaPropertyIdentityCache::resolve(
    const std::string& reference, std::string& error) {
    std::string runtimeId;
    if (reference.rfind("uia:", 0) == 0) {
        runtimeId = reference.substr(4);
        std::vector<int> parsed;
        if (!parse_runtime_id(runtimeId, parsed)) {
            error = "The UI Automation RuntimeId reference is malformed";
            return std::nullopt;
        }
    } else {
        const auto found = m_runtimeIdsByKey.find(reference);
        if (found == m_runtimeIdsByKey.end()) {
            error =
                "The UI Automation key is stale or was not returned by this session; "
                "refresh the originating raw/control/content tree and use its key or RuntimeId";
            return std::nullopt;
        }
        if (found->second.size() != 1) {
            error =
                "The UI Automation key is ambiguous across trees or views; "
                "refresh the originating tree and use uia:<RuntimeId>";
            return std::nullopt;
        }
        runtimeId = found->second.begin()->first;
    }

    auto existing = m_handlesByRuntimeId.find(runtimeId);
    if (existing != m_handlesByRuntimeId.end()) {
        existing->second.lastUsed = ++m_clock;
        return existing->second.handle;
    }

    std::unordered_set<std::string> currentRuntimeIds;
    for (const auto& [_, current] : m_currentRuntimeIdsByScope)
        currentRuntimeIds.insert(current.begin(), current.end());
    if (currentRuntimeIds.size() >= kMaximumRuntimeIds) {
        error =
            "The UI Automation RuntimeId cannot be retained because current "
            "session trees already fill the bounded identity cache; use a "
            "narrower view or element scope";
        return std::nullopt;
    }

    auto& scopeState = m_scopes[m_activeScope];
    if (scopeState.generation == 0)
        scopeState.generation = 1;
    scopeState.lastUsed = ++m_clock;

    const auto handle = m_nextHandle++;
    m_handlesByRuntimeId.emplace(
        runtimeId,
        RuntimeIdentity{
            .handle = handle,
            .lastUsed = ++m_clock,
            .lastSeenByScope = {
                {m_activeScope, scopeState.generation},
            },
        });
    m_runtimeIdsByHandle.emplace(handle, runtimeId);
    prune();
    return handle;
}

std::optional<std::string> UiaPropertyIdentityCache::runtime_id(
    uint64_t handle) const {
    const auto found = m_runtimeIdsByHandle.find(handle);
    if (found == m_runtimeIdsByHandle.end())
        return std::nullopt;
    return found->second;
}

size_t UiaPropertyIdentityCache::key_alias_count() const {
    size_t count = 0;
    for (const auto& [_, aliases] : m_runtimeIdsByKey)
        count += aliases.size();
    return count;
}

void UiaPropertyIdentityCache::collect_runtime_ids(
    const Element& element,
    std::unordered_set<std::string>& runtimeIds) const {
    const auto runtime = element.properties.find("RuntimeId");
    if (runtime != element.properties.end() && !runtime->second.empty())
        runtimeIds.insert(runtime->second);
    for (const auto& child : element.children)
        collect_runtime_ids(child, runtimeIds);
}

void UiaPropertyIdentityCache::collect_keys(
    const Element& element,
    std::unordered_set<std::string>& keys) const {
    if (!element.key.empty())
        keys.insert(element.key);
    for (const auto& child : element.children)
        collect_keys(child, keys);
}

void UiaPropertyIdentityCache::prune(
    const std::unordered_set<std::string>& extraProtectedRuntimeIds,
    const std::unordered_set<std::string>& extraProtectedKeys) {
    std::unordered_set<std::string> protectedRuntimeIds =
        extraProtectedRuntimeIds;
    for (const auto& [_, current] : m_currentRuntimeIdsByScope)
        protectedRuntimeIds.insert(current.begin(), current.end());
    std::unordered_set<std::string> protectedKeys = extraProtectedKeys;
    for (const auto& [_, current] : m_currentKeysByScope)
        protectedKeys.insert(current.begin(), current.end());
    const auto remove_runtime = [&](const std::string& runtimeId) {
        const auto found = m_handlesByRuntimeId.find(runtimeId);
        if (found == m_handlesByRuntimeId.end())
            return;
        m_runtimeIdsByHandle.erase(found->second.handle);
        m_handlesByRuntimeId.erase(found);
    };

    std::vector<std::string> staleRuntimeIds;
    for (const auto& [runtimeId, identity] : m_handlesByRuntimeId) {
        bool fresh = false;
        for (const auto& [scope, lastSeen] : identity.lastSeenByScope) {
            const auto state = m_scopes.find(scope);
            if (state != m_scopes.end() &&
                lastSeen + 1 >= state->second.generation) {
                fresh = true;
                break;
            }
        }
        if (!fresh && !protectedRuntimeIds.contains(runtimeId))
            staleRuntimeIds.push_back(runtimeId);
    }
    for (const auto& runtimeId : staleRuntimeIds)
        remove_runtime(runtimeId);

    while (m_handlesByRuntimeId.size() > kMaximumRuntimeIds) {
        auto oldest = m_handlesByRuntimeId.end();
        for (auto it = m_handlesByRuntimeId.begin();
             it != m_handlesByRuntimeId.end(); ++it) {
            if (protectedRuntimeIds.contains(it->first))
                continue;
            if (oldest == m_handlesByRuntimeId.end() ||
                it->second.lastUsed < oldest->second.lastUsed) {
                oldest = it;
            }
        }
        if (oldest == m_handlesByRuntimeId.end())
            break;
        const auto runtimeId = oldest->first;
        remove_runtime(runtimeId);
    }

    for (auto key = m_runtimeIdsByKey.begin();
         key != m_runtimeIdsByKey.end();) {
        for (auto alias = key->second.begin();
             alias != key->second.end();) {
            if (!m_handlesByRuntimeId.contains(alias->first))
                alias = key->second.erase(alias);
            else
                ++alias;
        }
        if (key->second.empty())
            key = m_runtimeIdsByKey.erase(key);
        else
            ++key;
    }

    while (key_alias_count() > kMaximumKeyAliases) {
        auto oldestKey = m_runtimeIdsByKey.end();
        std::unordered_map<std::string, uint64_t>::iterator oldestAlias;
        uint64_t oldestGeneration = (std::numeric_limits<uint64_t>::max)();
        for (auto key = m_runtimeIdsByKey.begin();
             key != m_runtimeIdsByKey.end(); ++key) {
            for (auto alias = key->second.begin();
                 alias != key->second.end(); ++alias) {
                if (protectedKeys.contains(key->first))
                    continue;
                if (alias->second < oldestGeneration) {
                    oldestGeneration = alias->second;
                    oldestKey = key;
                    oldestAlias = alias;
                }
            }
        }
        if (oldestKey == m_runtimeIdsByKey.end())
            break;
        oldestKey->second.erase(oldestAlias);
        if (oldestKey->second.empty())
            m_runtimeIdsByKey.erase(oldestKey);
    }
}

namespace {

bool has_supported_pattern(const Element& element, const char* wanted) {
    const auto found = element.properties.find("SupportedPatterns");
    if (found == element.properties.end())
        return false;
    size_t start = 0;
    while (start < found->second.size()) {
        const auto comma = found->second.find(',', start);
        const auto token = found->second.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (token == wanted)
            return true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

bool bool_property(const Element& element, const char* name, bool fallback) {
    const auto found = element.properties.find(name);
    if (found == element.properties.end())
        return fallback;
    return found->second == "true";
}

struct LocatedPropertyElement {
    const Element* element = nullptr;
    UiaSelectionCapabilities selection;
};

bool locate_property_element(
    const Element& current, uint64_t handle,
    UiaSelectionCapabilities inheritedSelection,
    LocatedPropertyElement& result) {
    if (has_supported_pattern(current, "Selection")) {
        inheritedSelection.known = true;
        inheritedSelection.canSelectMultiple =
            bool_property(current, "Selection.CanSelectMultiple", false);
        inheritedSelection.isSelectionRequired =
            bool_property(current, "Selection.IsSelectionRequired", true);
    }

    if (current.providerHandle == handle) {
        result.element = &current;
        result.selection = inheritedSelection;
        return true;
    }
    for (const auto& child : current.children) {
        if (locate_property_element(
                child, handle, inheritedSelection, result)) {
            return true;
        }
    }
    return false;
}

const PropertyDescriptor* find_descriptor(
    const PropertySchema& schema, const std::string& descriptorId) {
    for (const auto& descriptor : schema.descriptors) {
        if (descriptor.descriptorId == descriptorId)
            return &descriptor;
    }
    return nullptr;
}

bool is_choice(
    const PropertyDescriptor& descriptor, const std::string& value) {
    return std::any_of(
        descriptor.choices.begin(), descriptor.choices.end(),
        [&](const PropertyChoice& choice) { return choice.value == value; });
}

bool validate_number(
    const PropertyDescriptor& descriptor, const std::string& text,
    std::string& error) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        error = "The property value must be a finite number";
        return false;
    }
    if (descriptor.minimum && value < *descriptor.minimum) {
        error = "The property value is below the provider-supplied minimum";
        return false;
    }
    if (descriptor.maximum && value > *descriptor.maximum) {
        error = "The property value is above the provider-supplied maximum";
        return false;
    }
    return true;
}

} // namespace

UiaConnection::UiaConnection(UiaTargetIdentity identity)
    : m_hwnd(identity.hwnd), m_pid(identity.pid),
      m_identity(std::move(identity)),
      m_state(std::make_unique<State>(m_identity)) {
}

std::shared_ptr<UiaConnection> UiaConnection::connect(
    const UiaTargetIdentity& identity) {
    if (!identity.valid() ||
        !target_identity_matches(identity, nullptr)) {
        return {};
    }
    auto connection = std::shared_ptr<UiaConnection>(
        new UiaConnection(identity));
    if (!connection->m_state->connected()) {
        return {};
    }
    return connection;
}

std::shared_ptr<UiaConnection> UiaConnection::connect(
    HWND hwnd, DWORD expectedPid,
    uint64_t expectedProcessCreationIdentity) {
    const auto identity = capture_uia_target_identity(
        hwnd, expectedPid, expectedProcessCreationIdentity);
    return identity ? connect(*identity)
                    : std::shared_ptr<UiaConnection>{};
}

UiaConnection::~UiaConnection() = default;

bool UiaConnection::get_tree(Element& root, bool fastProperties,
                             const std::string& /*providerOption*/) {
    (void)fastProperties;
    return get_tree_with_options(root, UiaOptions{});
}

bool UiaConnection::get_tree_with_options(Element& root, const UiaOptions& options,
                                          bool* truncated) {
    if (truncated)
        *truncated = false;

    // All COM work is dispatched to the connection's one persistent MTA
    // thread. The operation lock serializes tree/property/event consumers
    // before they enqueue work, while the worker owns every COM reference.
    std::unique_lock<std::mutex> lock(m_state->operationMutex);
    if (!matches_target(m_hwnd) || !m_state->connected())
        return false;
    if (m_state->failNextTree.exchange(
            false, std::memory_order_acq_rel) ||
        fail_connected_tree_for_testing()) {
        return false;
    }

    Element built;
    bool wasTruncated = false;
    const HRESULT hr = m_state->invoke(
        [state = m_state.get(), &options, &built,
         &wasTruncated]() -> HRESULT {
            RETURN_HR_IF_NULL(E_FAIL, state->automation.get());
            apply_automation_timeouts(state->automation.get(), options);
            const HRESULT buildHr = build_tree_with_automation(
                state->automation.get(), state->targetIdentity,
                state->process.get(), nullptr, options, built,
                wasTruncated);
            if (is_uia_ownership_failure(buildHr))
                state->teardown_on_mta();
            return buildHr;
        });
    if (SUCCEEDED(hr)) {
        if (!m_state->identities.attach(
                built, identity_scope(options), !wasTruncated)) {
            m_state->identityError =
                "The UI Automation tree exceeds lvt's bounded identity capacity; "
                "use a narrower view or element scope";
            lock.unlock();
            return false;
        }
        m_state->identityError.clear();
    }
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

bool UiaConnection::attach_property_identities(
    Element& root, const UiaOptions& options, bool completeSnapshot) {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    if (!validate_target_identity_locked())
        return false;
    const bool attached = m_state->identities.attach(
        root, identity_scope(options), completeSnapshot);
    m_state->identityError = attached
        ? std::string()
        : "The UI Automation tree exceeds lvt's bounded identity capacity; "
          "use a narrower view or element scope";
    return attached;
}

bool UiaConnection::remember_property_references(
    const Element& root, const UiaOptions& options,
    bool completeSnapshot) {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    if (!validate_target_identity_locked())
        return false;
    const bool remembered =
        m_state->identities.remember(
            root, identity_scope(options), completeSnapshot);
    m_state->identityError = remembered
        ? std::string()
        : "The UI Automation tree has too many keys for lvt's bounded "
          "identity cache; use a narrower view or element scope";
    return remembered;
}

std::string UiaConnection::property_identity_error() {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    return m_state->identityError;
}

std::optional<uint64_t> UiaConnection::resolve_property_reference(
    const std::string& reference, std::string& error) {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    if (!validate_target_identity_locked()) {
        error = "The UI Automation connection no longer matches this session's target window";
        return std::nullopt;
    }

    return m_state->identities.resolve(reference, error);
}

bool UiaConnection::matches_target(HWND hwnd) const {
    return hwnd == m_hwnd &&
           target_identity_matches(m_identity, nullptr);
}

bool UiaConnection::validate_target_identity_locked() const {
    if (!m_state->connected() || !matches_target(m_hwnd))
        return false;
    const HRESULT hr = m_state->invoke(
        [state = m_state.get()]() -> HRESULT {
            wil::com_ptr<IUIAutomationElement> root;
            const HRESULT validateHr = get_validated_uia_root(
                state->automation.get(), state->targetIdentity,
                state->process.get(), &root);
            if (is_uia_ownership_failure(validateHr))
                state->teardown_on_mta();
            return validateHr;
        });
    return SUCCEEDED(hr);
}

PropertySnapshotResult UiaConnection::get_property_snapshot(uint64_t handle) {
    if (!matches_target(m_hwnd)) {
        PropertySnapshotResult result;
        result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        result.error =
            "The UI Automation connection no longer matches this session's target window";
        return result;
    }

    Element tree;
    bool refreshed = false;
    UiaOptions options;
    options.view = UiaView::raw;
    for (int attempt = 0; attempt < 3 && !refreshed; ++attempt) {
        if (attempt > 0)
            Sleep(static_cast<DWORD>(120 * attempt));
        refreshed = get_tree_with_options(tree, options);
    }
    if (!refreshed) {
        PropertySnapshotResult result;
        result.hresult = E_FAIL;
        result.error = "Could not refresh the UI Automation tree";
        return result;
    }

    LocatedPropertyElement located;
    if (!locate_property_element(
            tree, handle, UiaSelectionCapabilities{}, located) ||
        !located.element) {
        PropertySnapshotResult result;
        result.hresult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        result.error =
            "The UI Automation element is stale or does not belong to this session";
        return result;
    }

    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    return make_uia_property_snapshot(
        *located.element, located.selection, m_state->schemaCache);
}

PropertyMutationResult UiaConnection::set_property(
    uint64_t handle, const std::string& descriptorId,
    const std::string& value) {
    const auto snapshot = get_property_snapshot(handle);
    if (!snapshot.ok || !snapshot.schema) {
        PropertyMutationResult result;
        result.hresult = snapshot.hresult;
        result.error = snapshot.error;
        return result;
    }

    const auto* descriptor =
        find_descriptor(*snapshot.schema, descriptorId);
    if (!descriptor) {
        PropertyMutationResult result;
        result.hresult = E_INVALIDARG;
        result.error =
            "The property descriptor is unknown, stale, or belongs to a different UI Automation element";
        return result;
    }
    if (!descriptor->writable) {
        PropertyMutationResult result;
        result.hresult = E_ACCESSDENIED;
        result.error = "The UI Automation property descriptor is read-only";
        return result;
    }
    if (!descriptor->choices.empty() && !is_choice(*descriptor, value)) {
        PropertyMutationResult result;
        result.hresult = E_INVALIDARG;
        result.error =
            "The selected value is not one of the provider-supplied choices";
        return result;
    }
    if (descriptor->kind == PropertyEditorKind::number) {
        std::string error;
        if (!validate_number(*descriptor, value, error)) {
            PropertyMutationResult result;
            result.hresult = E_INVALIDARG;
            result.error = std::move(error);
            return result;
        }
    }

    UiaPropertyAction action;
    if (descriptor->name == "Value.Value") {
        action = UiaPropertyAction::setValue;
    } else if (descriptor->name == "RangeValue.Value") {
        action = UiaPropertyAction::setRangeValue;
    } else if (descriptor->name == "Toggle.ToggleState") {
        action = UiaPropertyAction::setToggleState;
    } else if (descriptor->name == "ExpandCollapse.State") {
        action = UiaPropertyAction::setExpandCollapseState;
    } else if (descriptor->name == "SelectionItem.Action") {
        if (value == "select")
            action = UiaPropertyAction::replaceSelection;
        else if (value == "add")
            action = UiaPropertyAction::addToSelection;
        else
            action = UiaPropertyAction::removeFromSelection;
    } else if (descriptor->name == "Scroll.Action") {
        action = UiaPropertyAction::scroll;
    } else {
        PropertyMutationResult result;
        result.hresult = E_INVALIDARG;
        result.error =
            "The property descriptor has no UI Automation mutation adapter";
        return result;
    }

    std::unique_lock<std::mutex> lock(m_state->operationMutex);
    if (!validate_target_identity_locked()) {
        PropertyMutationResult result;
        result.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        result.error =
            "The UI Automation connection no longer matches this session's target window";
        return result;
    }
    const auto runtime = m_state->identities.runtime_id(handle);
    if (!runtime) {
        PropertyMutationResult result;
        result.hresult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        result.error =
            "The UI Automation element or session is no longer available";
        return result;
    }
    std::vector<int> runtimeId;
    if (!parse_runtime_id(*runtime, runtimeId)) {
        PropertyMutationResult result;
        result.hresult = E_INVALIDARG;
        result.error = "The UI Automation element has an invalid RuntimeId";
        return result;
    }

    PropertyMutationResult result;
    const HRESULT hr = m_state->invoke([&, state = m_state.get()]() -> HRESULT {
        RETURN_HR_IF_NULL(E_FAIL, state->automation.get());
        result = perform_uia_property_action(
            state->automation.get(), state->targetIdentity,
            state->process.get(), runtimeId, action, value);
        return S_OK;
    });
    if (FAILED(hr)) {
        result.ok = false;
        result.hresult = hr;
        result.error = "The UI Automation property action could not run";
    }
    return result;
}

PropertyMutationResult UiaConnection::clear_property(
    uint64_t handle, const std::string& descriptorId) {
    const auto snapshot = get_property_snapshot(handle);
    if (!snapshot.ok || !snapshot.schema) {
        PropertyMutationResult result;
        result.hresult = snapshot.hresult;
        result.error = snapshot.error;
        return result;
    }
    if (!find_descriptor(*snapshot.schema, descriptorId)) {
        PropertyMutationResult result;
        result.hresult = E_INVALIDARG;
        result.error =
            "The property descriptor is unknown, stale, or belongs to a different UI Automation element";
        return result;
    }

    PropertyMutationResult result;
    result.hresult = E_NOTIMPL;
    result.error =
        "UI Automation does not expose a generic clear/reset operation for this property";
    return result;
}

std::vector<ConnectionEvent> UiaConnection::poll_events() {
    std::vector<ConnectionEvent> result;
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    if (!m_state->connected() || !matches_target(m_hwnd))
        return result;

    bool snapshotRequired = false;
    const HRESULT hr = m_state->invoke(
        [state = m_state.get(), &snapshotRequired]() -> HRESULT {
            wil::com_ptr<IUIAutomationElement> root;
            const HRESULT validateHr = get_validated_uia_root(
                state->automation.get(), state->targetIdentity,
                state->process.get(), &root);
            if (FAILED(validateHr)) {
                if (is_uia_ownership_failure(validateHr))
                    state->teardown_on_mta();
                return validateHr;
            }
            if (!state->eventRegistration.handler)
                return E_FAIL;
            snapshotRequired =
                state->eventRegistration.handler->consume();
            return S_OK;
        });
    if (SUCCEEDED(hr) && snapshotRequired) {
        ConnectionEvent event;
        event.mutation = ConnectionEvent::Mutation::snapshotRequired;
        result.push_back(std::move(event));
    }
    if (SUCCEEDED(hr)) {
        append_uia_event_test_stat(
            "poll", m_hwnd, m_pid, snapshotRequired);
    }
    return result;
}

bool UiaConnection::refresh_events() {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    if (!m_state->connected() || !matches_target(m_hwnd))
        return false;
    const bool refreshed = SUCCEEDED(m_state->invoke(
        [state = m_state.get()]() -> HRESULT {
            wil::com_ptr<IUIAutomationElement> root;
            const HRESULT validateHr = get_validated_uia_root(
                state->automation.get(), state->targetIdentity,
                state->process.get(), &root);
            if (is_uia_ownership_failure(validateHr))
                state->teardown_on_mta();
            return validateHr;
        }));
    if (refreshed)
        append_uia_event_test_stat("refresh", m_hwnd, m_pid);
    return refreshed;
}

bool UiaConnection::is_alive() const {
    std::lock_guard<std::mutex> lock(m_state->operationMutex);
    return validate_target_identity_locked();
}

void UiaConnection::fail_next_tree_for_testing() {
    m_state->failNextTree.store(
        true, std::memory_order_release);
}

} // namespace lvt
