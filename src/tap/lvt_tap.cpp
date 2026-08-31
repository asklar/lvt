// lvt_tap.cpp — TAP DLL for XAML diagnostics
// Injected into the target process by InitializeXamlDiagnosticsEx.
// Implements IObjectWithSite → receives IXamlDiagnostics → walks XAML tree
// via IVisualTreeService::AdviseVisualTreeChange → sends JSON over named pipe.

#include <Windows.h>
#include <atomic>
#include <objbase.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <optional>
#include <unknwn.h>

#include "xaml_property_filter.h"
#include "bounded_event_queue.h"
#include "xaml_enum_catalog.h"
#include "../xaml_enum_util.h"

// C++/WinRT projected types for XAML element inspection.
// System XAML (Windows.UI.Xaml) headers are always available from the Windows SDK.
// WinUI3 (Microsoft.UI.Xaml) headers are generated from the Windows App SDK winmd.
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>
#define LVT_HAS_XAML_PROJECTION 1

#if __has_include(<winrt/Microsoft.UI.Xaml.h>)
#define LVT_HAS_WINUI3_PROJECTION 1
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#else
#define LVT_HAS_WINUI3_PROJECTION 0
#endif

constexpr HRESULT
    LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION =
        static_cast<HRESULT>(0xA0040201u);

// Stub for C++/WinRT error origination (avoid linking windowsapp.lib)
extern "C" int32_t __stdcall WINRT_IMPL_RoOriginateLanguageException(
    int32_t error, void* message, void* exception) noexcept {
    return error;
}

// GUIDs only forward-declared in xamlOM.h (no .lib provides them)
const IID IID_IVisualTreeServiceCallback =
    { 0xAA7A8931, 0x80E4, 0x4FEC, { 0x8F, 0x3B, 0x55, 0x3F, 0x87, 0xB4, 0x96, 0x6E } };
const IID IID_IVisualTreeServiceCallback2 =
    { 0xBAD9EB88, 0xAE77, 0x4397, { 0xB9, 0x48, 0x5F, 0xA2, 0xDB, 0x0A, 0x19, 0xEA } };

static const CLSID CLSID_LvtTap =
    { 0xB8F3E2D1, 0x9A4C, 0x4F5E, { 0xB6, 0xD7, 0x8C, 0x1A, 0x3E, 0x5F, 0x7D, 0x9B } };

// Debug logging to file (since OutputDebugString may not be visible)
static void LogMsg(const char* fmt, ...) {
    static FILE* logFile = nullptr;
    if (!logFile) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wcscat_s(tmp, L"lvt_tap.log");
        logFile = _wfopen(tmp, L"a");
        if (!logFile) return;
    }
    fprintf(
        logFile, "[%llu][%lu][%lu] ", GetTickCount64(),
        GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fprintf(logFile, "\n");
    fflush(logFile);
}

static HMODULE GetCurrentModuleHandle() {
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &hm);
    return hm;
}

// Buffered line reader over the persistent duplex pipe. Reading one byte per
// ReadFile call (fine for a single one-shot handshake) is far too slow once
// this pipe stays open for the whole connection and carries a full tree's
// JSON (potentially several MB) as one line per GET_TREE response — this
// reads in 8KB chunks and splits on '\n', keeping any partial trailing line
// buffered for the next call.
class PipeLineReader {
public:
    explicit PipeLineReader(HANDLE pipe) : m_pipe(pipe) {}

    // Returns std::nullopt on EOF or a read error (the pipe is gone).
    std::optional<std::string> ReadLine() {
        for (;;) {
            auto nl = m_buffer.find('\n');
            if (nl != std::string::npos) {
                std::string line = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }
            char chunk[8192];
            DWORD read = 0;
            BOOL ok = ReadFile(m_pipe, chunk, sizeof(chunk), &read, nullptr);
            if (!ok || read == 0) return std::nullopt;
            m_buffer.append(chunk, read);
        }
    }

private:
    HANDLE m_pipe;
    std::string m_buffer;
};

struct TreeNode {
    InstanceHandle handle = 0;
    std::wstring type;
    std::wstring name;
    unsigned int numChildren = 0;
    InstanceHandle parent = 0;
    unsigned int childIndex = 0;
    std::vector<InstanceHandle> childHandles;
    std::vector<std::pair<std::wstring, std::wstring>> properties; // name, value
    double width = 0, height = 0;
    double offsetX = 0, offsetY = 0;
    bool hasBounds = false;
};

class LvtTap;

// Describes one chunk of nodes to collect, passed via SendMessage's LPARAM.
// The struct lives on AdviseThreadProcImpl's stack: SendMessage is
// synchronous, so it stays valid for exactly as long as the receiving
// thread's WndProc needs it, with no lifetime management required.
struct BatchRequest {
    LvtTap* self;
    size_t start;
    size_t count;
};

struct TapProperty {
    std::wstring name;
    std::wstring value;
    std::wstring propertyType;
    std::wstring valueType;
    std::wstring declaringType;
    unsigned int propertyIndex = 0;
    uint64_t metadataBits = 0;
    bool overridden = false;
    std::wstring source;
};

enum class TapPropertyCommandKind {
    getProperties,
    setProperty,
    clearProperty,
};

// Passed synchronously from the pipe worker to the SetSite/XAML UI thread via
// SendMessage. The request and result both remain on the worker's stack until
// the UI-thread call returns.
struct TapPropertyCommand {
    TapPropertyCommandKind kind = TapPropertyCommandKind::getProperties;
    uint64_t commandId = 0;
    InstanceHandle object = 0;
    std::vector<InstanceHandle> allowedRoots;
    bool simulateReparent = false;
    unsigned int propertyIndex = 0;
    std::wstring value;

    HRESULT hresult = E_FAIL;
    std::wstring error;
    std::vector<TapProperty> properties;
    bool hasReadback = false;
    TapProperty readback;
};

struct TapChangeEvent {
    bool added = false;
    InstanceHandle handle = 0;
    InstanceHandle parent = 0;
    unsigned int childIndex = 0;
    std::wstring type;
    std::wstring name;
};

// Forward declaration for WndProc
static LRESULT CALLBACK LvtTapMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

class LvtTap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
    LONG m_refCount = 1;
    wil::com_ptr<IUnknown> m_site;
    wil::com_ptr<IXamlDiagnostics> m_diag;
    wil::unique_hwnd m_msgWnd; // Message-only window for UI thread dispatch
    std::map<InstanceHandle, TreeNode> m_nodes;
    // Stable, flattened order over m_nodes' handles, built once per pass so
    // both collection functions can be dispatched to the UI thread in small
    // chunks (see AdviseThreadProcImpl) instead of one giant blocking call —
    // a std::map has no efficient random-access range, hence flattening.
    std::vector<InstanceHandle> m_orderedHandles;
    std::vector<InstanceHandle> m_roots;
    // Guards all direct access to m_nodes/m_roots/m_orderedHandles. XAML's
    // OnVisualTreeChange can fire on whichever UI thread owns the affected
    // element(s) - which, for an app with more than one XAML "core"/window,
    // need not be the same thread m_msgWnd (and therefore CollectBounds/
    // CollectPositionsAndText, dispatched there via SendMessage) is pinned
    // to. This mattered far less under the old one-shot-per-tick design,
    // which read this data exactly once, immediately after a single
    // synchronous initial replay. A persistent connection reads it
    // repeatedly across many GET_TREE requests over its whole life, so a
    // concurrent Add/Remove from another thread needs an actual lock, not
    // just favorable timing.
    std::mutex m_nodesMutex;
    std::wstring m_pipeName;
    // Parsed from the pipe-name suffix ("pipe_name|FAST") passed down from
    // xaml_diag_common.cpp — this is only the connection-wide *default*;
    // each GET_TREE request can override it for that one response (see
    // HandleGetTree), so a single persistent connection can still mix fast
    // live-tree polls with an occasional full-property request the way the
    // old per-call model did.
    bool m_fastMode = false;
    // The persistent, duplex connection back to lvt.exe. Opened exactly
    // once per connection lifetime (see ConnectPipeOnce) and kept open for
    // as long as the command loop runs - this is the whole point of this
    // redesign: one connect, many requests, instead of the old one-shot
    // "collect once, write once, close" pipe.
    wil::unique_hfile m_pipe;
    // All pipe writes are performed by the command-loop thread. The mutex is
    // retained because property/tree helpers also funnel through WriteLine,
    // and keeping one serialization point prevents future concurrent callers
    // from interleaving records.
    std::mutex m_pipeWriteMutex;
    std::atomic_bool m_acceptChangeEvents = false;
    static constexpr size_t kMaxChangeEvents = 4096;
    lvt::BoundedEventQueue<TapChangeEvent, kMaxChangeEvents> m_changeEvents;
    std::vector<lvt::tap::EnumTypeInfo> m_enumCatalog;
    lvt::XamlEnumFlagsCache m_enumFlagsCache;
    bool m_enumCatalogServed = false;

public:
    wil::com_ptr<IVisualTreeService> m_vts;
    static constexpr UINT WM_COLLECT_BOUNDS = WM_USER + 100;
    // WM_COLLECT_BOUNDS + 1 is used for CollectPositionsAndText dispatch
    // (see LvtTapMsgWndProc). This one asks the UI thread (the only thread
    // allowed to destroy a window it owns) to destroy m_msgWnd during final
    // cleanup - see CleanupUIResources.
    static constexpr UINT WM_TAP_DESTROY = WM_USER + 102;
    static constexpr UINT WM_PROPERTY_COMMAND = WM_USER + 103;

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else if (riid == IID_IVisualTreeServiceCallback) {
            *ppv = static_cast<IVisualTreeServiceCallback*>(
                static_cast<IVisualTreeServiceCallback2*>(this));
        } else if (riid == IID_IVisualTreeServiceCallback2) {
            *ppv = static_cast<IVisualTreeServiceCallback2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&m_refCount);
        if (c == 0) delete this;
        return c;
    }

    // IObjectWithSite — called by the XAML runtime on the UI thread
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pSite) override {
        LogMsg("SetSite called, pSite=%p", pSite);

        m_site.reset();
        m_vts.reset();
        m_diag.reset();

        if (pSite) {
            HRESULT hr = pSite->QueryInterface(IID_PPV_ARGS(m_site.put()));
            if (FAILED(hr)) {
                LogMsg("QI for IUnknown failed: 0x%08X", hr);
                return S_OK;
            }
        }

        if (!pSite) return S_OK;

        // Note: Windhawk calls FreeLibrary(GetCurrentModuleHandle()) here to balance
        // the refcount from InitializeXamlDiagnosticsEx. We skip this because our DLL
        // only has one LoadLibrary reference (unlike Windhawk which has two from its
        // hook mechanism). The DLL stays loaded in the target, which is acceptable.

        __try {
            return SetSiteImpl(pSite);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("SetSiteImpl crashed, code=0x%08X", GetExceptionCode());
            return E_FAIL;
        }
    }

    HRESULT SetSiteImpl(IUnknown* pSite) {
        wil::com_ptr<IXamlDiagnostics> diag;
        HRESULT hr = pSite->QueryInterface(IID_PPV_ARGS(diag.put()));
        if (FAILED(hr) || !diag) {
            LogMsg("QI for IXamlDiagnostics failed: 0x%08X", hr);
            return S_OK;
        }

        BSTR rawInitData = nullptr;
        diag->GetInitializationData(&rawInitData);
        wil::unique_bstr initData(rawInitData);
        if (initData) {
            std::wstring data(initData.get());
            // Format: "pipe_name" or "pipe_name|FAST"
            auto sep = data.find(L'|');
            if (sep != std::wstring::npos) {
                m_pipeName = data.substr(0, sep);
                std::wstring flags = data.substr(sep + 1);
                m_fastMode = (flags.find(L"FAST") != std::wstring::npos);
            } else {
                m_pipeName = data;
            }
            LogMsg("Pipe name: %ls, fastMode: %d", m_pipeName.c_str(), m_fastMode);
        }

        hr = diag->QueryInterface(IID_PPV_ARGS(m_vts.put()));
        if (FAILED(hr) || !m_vts) {
            LogMsg("QI for IVisualTreeService failed: 0x%08X", hr);
            return S_OK;
        }

        m_diag = std::move(diag);
        LoadEnumCatalog();

        // Create a message-only window on the UI thread for dispatching
        // GetPropertyValuesChain calls (which have thread affinity).
        WNDCLASSW wc = {};
        wc.lpfnWndProc = LvtTapMsgWndProc;
        wc.hInstance = GetCurrentModuleHandle();
        wc.lpszClassName = L"LvtTapMsg";
        RegisterClassW(&wc);
        m_msgWnd.reset(CreateWindowExW(0, L"LvtTapMsg", nullptr, 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr));
        if (m_msgWnd) {
            SetWindowLongPtrW(m_msgWnd.get(), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            LogMsg("Created message window %p on thread %lu", m_msgWnd.get(), GetCurrentThreadId());
        }

        // AdviseVisualTreeChange hangs if called on the SetSite thread.
        // Fire-and-forget a worker thread (same as Windhawk).
        AddRef();
        wil::com_ptr<LvtTap> threadSelf;
        threadSelf.attach(this);
        wil::unique_handle hThread(CreateThread(nullptr, 0, &AdviseThreadProc, threadSelf.get(), 0, nullptr));
        if (hThread) {
            (void)threadSelf.detach();
        } else {
            LogMsg("CreateThread failed: %lu", GetLastError());
        }

        return S_OK;
    }

    static DWORD WINAPI AdviseThreadProc(LPVOID param) {
        wil::com_ptr<LvtTap> self;
        self.attach(reinterpret_cast<LvtTap*>(param));
        return self->AdviseThreadProcImpl();
    }

    // Connects the persistent duplex pipe back to lvt.exe. Unlike the old
    // one-shot model (open, write once, close), this handle is kept open
    // for the connection's whole life - see m_pipe's comment.
    bool ConnectPipeOnce() {
        if (m_pipeName.empty()) {
            LogMsg("ConnectPipeOnce: pipe name is empty");
            return false;
        }
        m_pipe.reset(CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
        if (!m_pipe) {
            LogMsg("ConnectPipeOnce: failed to open pipe, error=%lu", GetLastError());
            return false;
        }
        LogMsg("ConnectPipeOnce: connected");
        return true;
    }

    // Writes one line (message + '\n') to the pipe.
    bool WriteLine(const std::string& utf8Line) {
        std::lock_guard<std::mutex> lock(m_pipeWriteMutex);
        if (!m_pipe) return false;
        std::string withNewline = utf8Line;
        withNewline += '\n';
        DWORD written = 0;
        BOOL ok = WriteFile(m_pipe.get(), withNewline.data(),
                             static_cast<DWORD>(withNewline.size()), &written, nullptr);
        if (ok) FlushFileBuffers(m_pipe.get());
        return ok != FALSE;
    }

    std::string SerializeChangeEvent(const TapChangeEvent& event) {
        std::wstring json = L"{\"type\":\"CHANGE\",\"mutation\":\"";
        json += event.added ? L"add" : L"remove";
        json += L"\",\"handle\":" + std::to_wstring(event.handle);
        json += L",\"parent\":" + std::to_wstring(event.parent);
        if (event.added) {
            json += L",\"childIndex\":" + std::to_wstring(event.childIndex);
            json += L",\"elementType\":\"" + Escape(event.type) + L"\"";
            if (!event.name.empty())
                json += L",\"name\":\"" + Escape(event.name) + L"\"";
        }
        json += L"}";

        int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                            utf8.data(), len, nullptr, nullptr);
        return utf8;
    }

    // Called from XAML's target UI callback. It must never perform pipe I/O,
    // flush a pipe, or wait for the reader. A bounded queue makes the callback
    // memory-only; overflow discards partial history and asks the consumer for
    // a fresh snapshot on the next command-thread drain.
    void QueueChangeEvent(bool added, InstanceHandle handle, InstanceHandle parent,
                          unsigned int childIndex, const std::wstring& type,
                          const std::wstring& name) {
        if (!m_acceptChangeEvents.load(std::memory_order_relaxed))
            return;

        m_changeEvents.push(
            TapChangeEvent{added, handle, parent, childIndex, type, name});
    }

    // Runs only on the command-loop thread. CHANGE records remain compatible
    // with the existing connection reader. EVENTS_OVERFLOW is an explicit
    // reset marker: consumers must treat queued deltas as incomplete and use
    // the next full GET_TREE snapshot as authoritative.
    void DrainChangeEvents() {
        auto drained = m_changeEvents.drain();

        if (drained.snapshotRequired)
            WriteLine("{\"type\":\"EVENTS_OVERFLOW\"}");
        for (const auto& event : drained.events)
            WriteLine(SerializeChangeEvent(event));
    }

    static bool ParseUint64(const std::string& text, uint64_t& value) {
        if (text.empty())
            return false;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    }

    static bool ParseAllowedRoots(
        const std::string& text,
        std::vector<InstanceHandle>& roots,
        bool& simulateReparent) {
        roots.clear();
        simulateReparent = false;
        std::string_view encoded = text;
        if (!encoded.empty() && encoded.front() == '!') {
            simulateReparent = true;
            encoded.remove_prefix(1);
        }
        if (encoded == "-")
            return true;
        size_t start = 0;
        while (start < encoded.size()) {
            const size_t separator = encoded.find(',', start);
            const size_t end = separator == std::string::npos
         ? encoded.size()
         : separator;
            uint64_t root = 0;
            if (end == start ||
         !ParseUint64(
             std::string(
                 encoded.substr(
                     start, end - start)),
             root) ||
         root == 0) {
         return false;
            }
            roots.push_back(
         static_cast<InstanceHandle>(root));
            if (separator == std::string::npos)
         break;
            start = separator + 1;
        }
        return !roots.empty();
    }

    static bool DecodeHexUtf8(const std::string& encoded, std::wstring& value) {
        if (encoded == "-") {
            value.clear();
            return true;
        }
        if (encoded.empty() || (encoded.size() % 2) != 0)
            return false;

        std::string utf8(encoded.size() / 2, '\0');
        auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < utf8.size(); ++i) {
            int high = nibble(encoded[i * 2]);
            int low = nibble(encoded[i * 2 + 1]);
            if (high < 0 || low < 0)
                return false;
            utf8[i] = static_cast<char>((high << 4) | low);
        }

        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
        if (length <= 0)
            return false;
        value.resize(length);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   utf8.data(), static_cast<int>(utf8.size()),
                                   value.data(), length) == length;
    }

    void LoadEnumCatalog() {
        unsigned int count = 0;
        EnumType* rawEnums = nullptr;
        const HRESULT getHr = m_vts->GetEnums(&count, &rawEnums);
        lvt::tap::OwnedEnumTypes owned(rawEnums, count);
        LogMsg("GetEnums called once: hr=0x%08X count=%u", getHr, count);
        if (FAILED(getHr))
            return;

        std::vector<lvt::tap::EnumTypeInfo> copied;
        const HRESULT copyHr =
            lvt::tap::copy_enum_types(owned.get(), owned.count(), copied);
        if (FAILED(copyHr)) {
            LogMsg("GetEnums deep copy failed: 0x%08X", copyHr);
            return;
        }
        for (auto& type : copied)
            type.flagsKind = m_enumFlagsCache.classify(type.name);
        m_enumCatalog = std::move(copied);
    }

    const lvt::tap::EnumTypeInfo* FindEnumType(
        const std::wstring& typeName) const {
        auto found = std::find_if(
            m_enumCatalog.begin(), m_enumCatalog.end(),
            [&](const lvt::tap::EnumTypeInfo& type) {
                return type.name == typeName;
            });
        return found == m_enumCatalog.end() ? nullptr : &*found;
    }

    static const wchar_t* FlagsKindNameWide(
        lvt::XamlEnumFlagsKind kind) {
        switch (kind) {
        case lvt::XamlEnumFlagsKind::nonFlags: return L"no";
        case lvt::XamlEnumFlagsKind::flags: return L"yes";
        case lvt::XamlEnumFlagsKind::unknown: return L"unknown";
        }
        return L"unknown";
    }

    static std::wstring TakeBstr(BSTR& value) {
        wil::unique_bstr owned(value);
        value = nullptr;
        return owned ? std::wstring(owned.get(), SysStringLen(owned.get())) : std::wstring();
    }

    static void SanitizeTypeName(std::wstring& value) {
        value.erase(std::remove_if(
            value.begin(), value.end(),
            [](wchar_t ch) { return ch < 0x20; }), value.end());
    }

    static const wchar_t* BaseValueSourceName(BaseValueSource source) {
        switch (source) {
        case BaseValueSourceDefault: return L"Default";
        case BaseValueSourceBuiltInStyle: return L"BuiltInStyle";
        case BaseValueSourceStyle: return L"Style";
        case BaseValueSourceLocal: return L"Local";
        case Inherited: return L"Inherited";
        case DefaultStyleTrigger: return L"DefaultStyleTrigger";
        case TemplateTrigger: return L"TemplateTrigger";
        case StyleTrigger: return L"StyleTrigger";
        case ImplicitStyleReference: return L"ImplicitStyleReference";
        case ParentTemplate: return L"ParentTemplate";
        case ParentTemplateTrigger: return L"ParentTemplateTrigger";
        case Animation: return L"Animation";
        case Coercion: return L"Coercion";
        case BaseValueSourceVisualState: return L"VisualState";
        default: return L"Unknown";
        }
    }

    HRESULT CollectPropertyChain(InstanceHandle object,
                                 std::vector<TapProperty>& output,
                                 std::wstring& error) {
        unsigned int sourceCount = 0;
        unsigned int propertyCount = 0;
        PropertyChainSource* rawSources = nullptr;
        PropertyChainValue* rawProperties = nullptr;
        HRESULT hr = m_vts->GetPropertyValuesChain(
            object, &sourceCount, &rawSources, &propertyCount, &rawProperties);
        wil::unique_cotaskmem sourcesMemory(rawSources);
        wil::unique_cotaskmem propertiesMemory(rawProperties);
        if (FAILED(hr)) {
            error = L"GetPropertyValuesChain failed";
            return hr;
        }

        std::vector<std::wstring> sources;
        sources.reserve(sourceCount);
        for (unsigned int i = 0; i < sourceCount; ++i) {
            auto targetType = TakeBstr(rawSources[i].TargetType);
            auto name = TakeBstr(rawSources[i].Name);
            auto fileName = TakeBstr(rawSources[i].SrcInfo.FileName);
            auto hash = TakeBstr(rawSources[i].SrcInfo.Hash);
            (void)targetType;
            (void)fileName;
            (void)hash;

            std::wstring label = BaseValueSourceName(rawSources[i].Source);
            if (!name.empty())
                label += L": " + name;
            sources.push_back(std::move(label));
        }

        std::set<std::wstring> seenNames;
        output.reserve(propertyCount);
        for (unsigned int i = 0; i < propertyCount; ++i) {
            auto type = TakeBstr(rawProperties[i].Type);
            auto declaringType = TakeBstr(rawProperties[i].DeclaringType);
            auto valueType = TakeBstr(rawProperties[i].ValueType);
            auto itemType = TakeBstr(rawProperties[i].ItemType);
            auto value = TakeBstr(rawProperties[i].Value);
            auto name = TakeBstr(rawProperties[i].PropertyName);
            (void)itemType;
            SanitizeTypeName(type);
            SanitizeTypeName(declaringType);
            SanitizeTypeName(valueType);

            // xamlOM orders the chain most-specific first. Preserve that
            // occurrence and discard later inherited/style duplicates.
            if (name.empty() || !seenNames.insert(name).second)
                continue;

            TapProperty property;
            property.name = std::move(name);
            property.value = std::move(value);
            property.propertyType = std::move(type);
            property.valueType = std::move(valueType);
            property.declaringType = std::move(declaringType);
            property.propertyIndex = rawProperties[i].Index;
            property.metadataBits = static_cast<uint64_t>(rawProperties[i].MetadataBits);
            property.overridden = rawProperties[i].Overridden != FALSE;
            if (rawProperties[i].PropertyChainIndex < sources.size())
                property.source = sources[rawProperties[i].PropertyChainIndex];
            output.push_back(std::move(property));
        }
        return S_OK;
    }

    bool ReadBackProperty(
        TapPropertyCommand& command, const wchar_t* operation) {
        std::vector<TapProperty> refreshedProperties;
        std::wstring readbackError;
        const HRESULT readbackHr = CollectPropertyChain(
            command.object, refreshedProperties, readbackError);
        if (FAILED(readbackHr)) {
            command.hresult = readbackHr;
            command.error =
                std::wstring(operation) +
                L" succeeded, but effective-value readback failed";
            if (!readbackError.empty())
                command.error += L": " + readbackError;
            return false;
        }

        const auto refreshed = std::find_if(
            refreshedProperties.begin(), refreshedProperties.end(),
            [&](const TapProperty& candidate) {
                return candidate.propertyIndex == command.propertyIndex;
            });
        if (refreshed == refreshedProperties.end()) {
            command.hresult = E_FAIL;
            command.error =
                std::wstring(operation) +
                L" succeeded, but the property was absent from effective-value readback";
            return false;
        }

        command.readback = *refreshed;
        command.hasReadback = true;
        command.hresult = S_OK;
        return true;
    }

    bool ValidatePropertyRoot(
        TapPropertyCommand& command,
        const wchar_t* operation) {
        if (command.allowedRoots.empty()) {
            command.hresult =
                LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION;
            command.error =
                std::wstring(operation) +
                L" has no authorized XAML root";
            return false;
        }

        std::lock_guard<std::mutex> lock(m_nodesMutex);
        auto current = m_nodes.find(command.object);
        if (current == m_nodes.end()) {
            command.hresult =
                LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION;
            command.error =
                std::wstring(operation) +
                L" target is detached or no longer tracked";
            return false;
        }

        std::set<InstanceHandle> visited;
        InstanceHandle root = current->first;
        while (current->second.parent != 0) {
            if (!visited.insert(root).second) {
                command.hresult =
                    LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION;
                command.error =
                    std::wstring(operation) +
                    L" target has an invalid parent cycle";
                return false;
            }
            root = current->second.parent;
            current = m_nodes.find(root);
            if (current == m_nodes.end()) {
                command.hresult =
                    LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION;
                command.error =
                    std::wstring(operation) +
                    L" target has a detached parent chain";
                return false;
            }
        }

        if (std::find(
                command.allowedRoots.begin(),
                command.allowedRoots.end(),
                root) == command.allowedRoots.end()) {
            command.hresult =
                LVT_E_PROPERTY_TARGET_OUTSIDE_SESSION;
            command.error =
                std::wstring(operation) +
                L" target no longer belongs to an authorized XAML root";
            return false;
        }
        return true;
    }

    void ApplyPropertyReparentForTesting(
        const TapPropertyCommand& command) {
        if (!command.simulateReparent)
            return;

        std::lock_guard<std::mutex> lock(m_nodesMutex);
        auto target = m_nodes.find(command.object);
        if (target == m_nodes.end() ||
            target->second.parent ==
                static_cast<InstanceHandle>(~0ull)) {
            return;
        }
        const InstanceHandle oldParent =
            target->second.parent;
        if (oldParent != 0) {
            auto parent = m_nodes.find(oldParent);
            if (parent != m_nodes.end()) {
                auto& children =
                    parent->second.childHandles;
                children.erase(
                    std::remove(
                        children.begin(), children.end(),
                        command.object),
                    children.end());
            }
        } else {
            m_roots.erase(
                std::remove(
                    m_roots.begin(), m_roots.end(),
                    command.object),
                m_roots.end());
        }

        const InstanceHandle syntheticRoot =
            static_cast<InstanceHandle>(~0ull);
        TreeNode root;
        root.handle = syntheticRoot;
        root.type =
            L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource";
        root.parent = 0;
        root.numChildren = 1;
        root.childHandles.push_back(command.object);
        m_nodes[syntheticRoot] = std::move(root);
        target = m_nodes.find(command.object);
        target->second.parent = syntheticRoot;
        m_roots.push_back(syntheticRoot);
    }

    void ExecutePropertyCommand(TapPropertyCommand& command) {
        if (!m_vts) {
            command.hresult = E_NOINTERFACE;
            command.error = L"IVisualTreeService is unavailable";
            return;
        }
        ApplyPropertyReparentForTesting(command);
        if (!ValidatePropertyRoot(
                command, L"Property operation")) {
            return;
        }

        if (command.kind == TapPropertyCommandKind::getProperties) {
            command.hresult = CollectPropertyChain(
                command.object, command.properties, command.error);
            if (SUCCEEDED(command.hresult)) {
                ValidatePropertyRoot(
                    command, L"Property readback");
            }
            return;
        }

        if (command.kind == TapPropertyCommandKind::setProperty ||
            command.kind == TapPropertyCommandKind::clearProperty) {
            std::vector<TapProperty> currentProperties;
            std::wstring metadataError;
            command.hresult = CollectPropertyChain(
                command.object, currentProperties, metadataError);
            if (FAILED(command.hresult)) {
                command.error = metadataError;
                return;
            }
            auto property = std::find_if(
                currentProperties.begin(), currentProperties.end(),
                [&](const TapProperty& candidate) {
                    return candidate.propertyIndex == command.propertyIndex;
                });
            if (property == currentProperties.end()) {
                command.hresult = E_INVALIDARG;
                command.error = L"The property descriptor is stale or does not apply to this object";
                return;
            }
            if ((property->metadataBits & IsPropertyReadOnly) != 0) {
                command.hresult = E_ACCESSDENIED;
                command.error = L"Property mutation refused a read-only property";
                return;
            }

            if (command.kind == TapPropertyCommandKind::clearProperty) {
                command.hresult = m_vts->ClearProperty(
                    command.object, command.propertyIndex);
                if (FAILED(command.hresult)) {
                    command.error = L"ClearProperty failed";
                    return;
                }
                if (!ValidatePropertyRoot(
                        command, L"ClearProperty readback")) {
                    return;
                }
                ReadBackProperty(command, L"ClearProperty");
                if (SUCCEEDED(command.hresult)) {
                    ValidatePropertyRoot(
                        command,
                        L"ClearProperty completed readback");
                }
                return;
            }

            if (property->propertyType.empty()) {
                command.hresult = E_INVALIDARG;
                command.error = L"The dependency property's declared type is unavailable";
                return;
            }

            if (const auto* enumType = FindEnumType(property->propertyType)) {
                const auto canonical =
                    lvt::detail::canonicalize_enum_member_list(
                        command.value, enumType->members,
                        enumType->flagsKind !=
                            lvt::XamlEnumFlagsKind::nonFlags);
                if (!canonical) {
                    command.hresult = E_INVALIDARG;
                    command.error =
                        L"The value contains a member not present in enum type '" +
                        property->propertyType + L"'";
                    return;
                }
                command.value = *canonical;
            }

            wil::unique_bstr type(SysAllocStringLen(
                property->propertyType.data(),
                static_cast<UINT>(property->propertyType.size())));
            wil::unique_bstr value(SysAllocStringLen(
                command.value.data(), static_cast<UINT>(command.value.size())));
            if (!type || !value) {
                command.hresult = E_OUTOFMEMORY;
                command.error = L"CreateInstance input allocation failed";
                return;
            }

            InstanceHandle valueHandle = 0;
            command.hresult = m_vts->CreateInstance(type.get(), value.get(), &valueHandle);
            if (FAILED(command.hresult)) {
                command.error = L"CreateInstance failed for value type '" +
                                property->propertyType + L"' and value '" +
                                command.value + L"'";
                return;
            }

            command.hresult = m_vts->SetProperty(
                command.object, valueHandle, command.propertyIndex);
            if (FAILED(command.hresult)) {
                command.error = L"SetProperty failed after CreateInstance succeeded";
                return;
            }
            if (!ValidatePropertyRoot(
                    command, L"SetProperty readback")) {
                return;
            }
            ReadBackProperty(command, L"SetProperty");
            if (SUCCEEDED(command.hresult)) {
                ValidatePropertyRoot(
                    command,
                    L"SetProperty completed readback");
            }
            return;
        }
    }

    std::wstring SerializePropertyResult(const TapPropertyCommand& command) {
        wchar_t hrText[16];
        swprintf_s(hrText, L"0x%08X", static_cast<unsigned int>(command.hresult));
        const bool ok = SUCCEEDED(command.hresult);
        std::wstring json = L"{\"type\":\"PROPERTY_RESULT\",\"commandId\":" +
                            std::to_wstring(command.commandId) +
                            L",\"ok\":" + (ok ? L"true" : L"false") +
                            L",\"hresult\":\"" + hrText + L"\"";
        if (!ok) {
            json += L",\"error\":\"" + Escape(
                command.error.empty() ? L"Property command failed" : command.error) + L"\"";
        } else if (command.kind == TapPropertyCommandKind::getProperties) {
            json += L",\"properties\":[";
            for (size_t i = 0; i < command.properties.size(); ++i) {
                if (i) json += L",";
                const auto& property = command.properties[i];
                json += L"{\"name\":\"" + Escape(property.name) +
                        L"\",\"value\":\"" + Escape(property.value) +
                        L"\",\"propertyType\":\"" + Escape(property.propertyType) +
                        L"\",\"valueType\":\"" + Escape(property.valueType) +
                        L"\",\"declaringType\":\"" + Escape(property.declaringType) +
                        L"\",\"propertyIndex\":" + std::to_wstring(property.propertyIndex) +
                        L",\"metadataBits\":" + std::to_wstring(property.metadataBits) +
                        L",\"overridden\":" + (property.overridden ? L"true" : L"false") +
                        L",\"source\":\"" + Escape(property.source) + L"\"}";
            }
            json += L"]";
        } else if (command.hasReadback) {
            json += L",\"readback\":{\"value\":\"" +
                    Escape(command.readback.value) +
                    L"\",\"propertyType\":\"" +
                    Escape(command.readback.propertyType) +
                    L"\",\"valueType\":\"" +
                    Escape(command.readback.valueType) +
                    L"\",\"overridden\":" +
                    (command.readback.overridden ? L"true" : L"false") +
                    L",\"source\":\"" +
                    Escape(command.readback.source) + L"\"}";
        }
        json += L"}";
        return json;
    }

    void WritePropertyResult(const TapPropertyCommand& command) {
        std::wstring response = SerializePropertyResult(command);
        int length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, response.data(),
            static_cast<int>(response.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            LogMsg("WritePropertyResult: response was not valid UTF-16");
            return;
        }
        std::string utf8(length, '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, response.data(),
            static_cast<int>(response.size()), utf8.data(), length, nullptr, nullptr);
        WriteLine(utf8);
    }

    void HandlePropertyCommand(const std::string& line) {
        TapPropertyCommand command;
        std::istringstream tokens(line);
        std::string verb;
        std::string commandIdText;
        std::string objectText;
        tokens >> verb >> commandIdText >> objectText;

        uint64_t object = 0;
        if (!ParseUint64(commandIdText, command.commandId) ||
            !ParseUint64(objectText, object) || object == 0) {
            command.hresult = E_INVALIDARG;
            command.error = L"Malformed property command id or object handle";
            WritePropertyResult(command);
            return;
        }
        command.object = static_cast<InstanceHandle>(object);

        std::string rootsText;
        tokens >> rootsText;
        if (!ParseAllowedRoots(
                rootsText, command.allowedRoots,
                command.simulateReparent)) {
            command.hresult = E_INVALIDARG;
            command.error =
                L"Malformed authorized XAML root list";
            WritePropertyResult(command);
            return;
        }

        if (verb == "GET_PROPERTIES") {
            command.kind = TapPropertyCommandKind::getProperties;
        } else {
            std::string indexText;
            uint64_t index = 0;
            tokens >> indexText;
            if (!ParseUint64(indexText, index) || index > UINT_MAX) {
                command.hresult = E_INVALIDARG;
                command.error = L"Malformed property index";
                WritePropertyResult(command);
                return;
            }
            command.propertyIndex = static_cast<unsigned int>(index);
            if (verb == "SET_PROPERTY") {
                command.kind = TapPropertyCommandKind::setProperty;
                std::string encodedValue;
                tokens >> encodedValue;
                if (!DecodeHexUtf8(encodedValue, command.value)) {
                    command.hresult = E_INVALIDARG;
                    command.error = L"SET_PROPERTY contains an invalid UTF-8 value";
                    WritePropertyResult(command);
                    return;
                }
            } else {
                command.kind = TapPropertyCommandKind::clearProperty;
            }
        }

        std::string extra;
        if (tokens >> extra) {
            command.hresult = E_INVALIDARG;
            command.error = L"Property command has unexpected trailing fields";
            WritePropertyResult(command);
            return;
        }

        if (!m_msgWnd) {
            command.hresult = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
            command.error = L"XAML UI-thread dispatcher is unavailable";
        } else {
            SendMessageW(m_msgWnd.get(), WM_PROPERTY_COMMAND, 0,
                         reinterpret_cast<LPARAM>(&command));
        }
        WritePropertyResult(command);
    }

    void HandlePollEvents(const std::string& line) {
        std::istringstream tokens(line);
        std::string verb;
        std::string commandIdText;
        std::string extra;
        uint64_t commandId = 0;
        tokens >> verb >> commandIdText;
        if (verb != "POLL_EVENTS" || !ParseUint64(commandIdText, commandId) ||
            (tokens >> extra)) {
            LogMsg("HandlePollEvents: malformed command");
            return;
        }
        DrainChangeEvents();
        WriteLine("{\"type\":\"EVENTS_RESULT\",\"commandId\":" +
                  std::to_string(commandId) + "}");
    }

    void HandleGetEnums(const std::string& line) {
        std::istringstream tokens(line);
        std::string verb;
        std::string commandIdText;
        std::string extra;
        uint64_t commandId = 0;
        tokens >> verb >> commandIdText;
        if (verb != "GET_ENUMS" || !ParseUint64(commandIdText, commandId) ||
            (tokens >> extra)) {
            LogMsg("HandleGetEnums: malformed command");
            return;
        }

        if (m_enumCatalogServed) {
            WriteLine(
                "{\"type\":\"ENUM_RESULT\",\"commandId\":" +
                std::to_string(commandId) +
                ",\"ok\":false,\"error\":\"Enum catalog was already served\"}");
            return;
        }
        m_enumCatalogServed = true;

        std::wstring response =
            L"{\"type\":\"ENUM_RESULT\",\"commandId\":" +
            std::to_wstring(commandId) +
            L",\"ok\":true,\"catalog\":[";
        for (size_t i = 0; i < m_enumCatalog.size(); ++i) {
            if (i)
                response += L",";
            const auto& type = m_enumCatalog[i];
            response += L"{\"name\":\"" + Escape(type.name) +
                        L"\",\"flags\":\"" +
                        FlagsKindNameWide(type.flagsKind) +
                        L"\"" +
                        L",\"members\":[";
            for (size_t j = 0; j < type.members.size(); ++j) {
                if (j)
                    response += L",";
                const auto& member = type.members[j];
                response +=
                    L"{\"machineValue\":" +
                    std::to_wstring(member.machineValue) +
                    L",\"name\":\"" + Escape(member.name) + L"\"}";
            }
            response += L"]}";
        }
        response += L"]}";

        int length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, response.data(),
            static_cast<int>(response.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            LogMsg("HandleGetEnums: response was not valid UTF-16");
            return;
        }
        std::string utf8(length, '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, response.data(),
            static_cast<int>(response.size()), utf8.data(), length,
            nullptr, nullptr);
        WriteLine(utf8);
    }

    DWORD AdviseThreadProcImpl() {
        LogMsg("AdviseThread starting");

        __try {
            IVisualTreeServiceCallback* cb =
                static_cast<IVisualTreeServiceCallback*>(
                    static_cast<IVisualTreeServiceCallback2*>(this));

            // AdviseVisualTreeChange is called exactly ONCE per connection
            // lifetime, here, and stays registered for as long as the
            // command loop below runs - it is a subscribe-and-react API
            // (OnVisualTreeChange keeps incrementally maintaining m_nodes/
            // m_roots for the whole connection), not something meant to be
            // re-established on every tree refresh. Re-subscribing from
            // scratch every poll (the old design) is what caused a
            // confirmed, unbounded per-tick resource leak - see
            // CleanupUIResources's comment for how this now cleans up
            // exactly once, when the connection actually ends, instead.
            LogMsg("Calling AdviseVisualTreeChange");
            HRESULT hr = m_vts->AdviseVisualTreeChange(cb);
            LogMsg("AdviseVisualTreeChange returned 0x%08X, nodes=%zu, roots=%zu",
                   hr, m_nodes.size(), m_roots.size());

            if (SUCCEEDED(hr)) {
                if (m_nodes.empty()) {
                    Sleep(500);
                    LogMsg("After sleep: nodes=%zu", m_nodes.size());
                }

                ServeConnection();

                m_vts->UnadviseVisualTreeChange(cb);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("AdviseThread crashed: 0x%08X", GetExceptionCode());
        }

        CleanupUIResources();
        LogMsg("AdviseThread exiting");
        return 0;
    }

    // Connects the persistent pipe and, if that succeeds, serves requests
    // until the connection ends. Split out of AdviseThreadProcImpl (rather
    // than inlined into its __try block) because MSVC's SEH rejects any
    // C++ temporary requiring unwind cleanup - such as the std::string
    // WriteLine("READY") would otherwise construct - directly inside a
    // function that also contains __try (error C2712); a plain function
    // call like this one has no such temporary at the __try call site.
    void ServeConnection() {
        if (ConnectPipeOnce()) {
            WriteLine("READY");
            m_acceptChangeEvents.store(true, std::memory_order_release);
            LogMsg("Sent READY, entering command loop");
            RunCommandLoop();
            m_acceptChangeEvents.store(false, std::memory_order_release);
        } else {
            LogMsg("Failed to connect pipe; cannot serve requests this connection");
        }
    }

    // Persistent request/response loop, run for as long as lvt.exe keeps
    // this connection open. Each request re-walks bounds/properties over
    // the ALREADY-subscribed tree (no re-injection, no new AdviseVisualTreeChange,
    // no new message window) - this is the entire point of this redesign.
    void RunCommandLoop() {
        PipeLineReader reader(m_pipe.get());
        for (;;) {
            auto line = reader.ReadLine();
            if (!line) {
                LogMsg("RunCommandLoop: pipe closed/error, exiting loop");
                break;
            }
            LogMsg("RunCommandLoop: received command '%s'", line->c_str());
            if (*line == "DISCONNECT") {
                WriteLine("BYE");
                break;
            } else if (line->rfind("GET_TREE", 0) == 0) {
                bool fast = line->find("FAST") != std::string::npos;
                HandleGetTree(fast);
            } else if (line->rfind("GET_PROPERTIES ", 0) == 0 ||
                       line->rfind("SET_PROPERTY ", 0) == 0 ||
                       line->rfind("CLEAR_PROPERTY ", 0) == 0) {
                HandlePropertyCommand(*line);
            } else if (line->rfind("POLL_EVENTS ", 0) == 0) {
                HandlePollEvents(*line);
            } else if (line->rfind("GET_ENUMS ", 0) == 0) {
                HandleGetEnums(*line);
            } else {
                LogMsg("RunCommandLoop: unknown command, ignoring");
            }
        }
    }

    // Re-collects bounds/properties for the tree already tracked in m_nodes
    // (kept current by OnVisualTreeChange for the whole connection) and
    // writes exactly one response line - every GET_TREE must get a
    // response, even an empty "[]", since lvt.exe blocks waiting for one.
    void HandleGetTree(bool fast) {
        m_fastMode = fast;
        DrainChangeEvents();

        {
            std::lock_guard<std::mutex> lock(m_nodesMutex);
            m_orderedHandles.clear();
            m_orderedHandles.reserve(m_nodes.size());
            for (auto& [handle, node] : m_nodes) {
                m_orderedHandles.push_back(handle);

                // Properties and geometry are a per-request snapshot, not
                // persistent tree identity. Leaving them in m_nodes made
                // Text/Content entries append again on every watch tick,
                // growing the serialized payload without bound. It also
                // left hasBounds true forever, so fast mode stopped reading
                // ActualWidth/ActualHeight after the first request and
                // returned stale sizes after a resize.
                node.properties.clear();
                node.width = 0;
                node.height = 0;
                node.offsetX = 0;
                node.offsetY = 0;
                node.hasBounds = false;
            }
        }

        // Dispatch GetPropertyValuesChain (and, below, TransformToVisual) to
        // the UI thread in small chunks rather than one call covering every
        // node. A single unbroken SendMessage call occupies the target's UI
        // thread start to finish (several seconds for a rich tree) with no
        // chance to service its own pending messages in between — including
        // the modal loop DefWindowProc runs while the user is dragging the
        // window, observed live as the target app feeling laggy/stuttery to
        // move while `watch` was attached. Chunking with a short sleep
        // between SendMessage calls lets that message queue drain between
        // chunks; every node still gets collected, in the same order, every
        // request.
        constexpr size_t kBatchSize = 20;
        if (m_msgWnd && !fast) {
            LogMsg("Dispatching CollectBounds to UI thread via SendMessage, %zu nodes in batches of %zu",
                   m_orderedHandles.size(), kBatchSize);
            for (size_t start = 0; start < m_orderedHandles.size(); start += kBatchSize) {
                BatchRequest req{this, start, kBatchSize};
                SendMessageW(m_msgWnd.get(), WM_COLLECT_BOUNDS, 0,
                             reinterpret_cast<LPARAM>(&req));
                Sleep(1);
            }
            LogMsg("Finished CollectBounds dispatch");
        } else if (m_msgWnd) {
            // CollectBounds itself is intentionally a no-op in fast mode,
            // but dispatching one SendMessage + Sleep per 20-node batch
            // still cost ~1.9 seconds for Microsoft Store's ~2400-node
            // tree. Skip the loop itself, not merely its per-node work.
            LogMsg("Skipped CollectBounds dispatch entirely in fast mode");
        }
        // Get element positions via TransformToVisual (works around broken
        // ActualOffset serialization in WinUI3). Must run on the UI thread.
#if LVT_HAS_XAML_PROJECTION
        if (m_msgWnd) {
            for (size_t start = 0; start < m_orderedHandles.size(); start += kBatchSize) {
                BatchRequest req{this, start, kBatchSize};
                SendMessageW(m_msgWnd.get(), WM_COLLECT_BOUNDS + 1, 0,
                             reinterpret_cast<LPARAM>(&req));
                Sleep(1);
            }
            LogMsg("Finished CollectPositionsAndText dispatch");
        }
#endif
        SerializeAndSend();
    }

    // Tears down everything SetSiteImpl/AdviseThreadProcImpl set up, exactly
    // once, when the connection actually ends (DISCONNECT or a broken
    // pipe) - not once per tree refresh. This is the direct fix for the
    // confirmed leak: every earlier version of this file created a new
    // message-only window per collection and never destroyed it.
    void CleanupUIResources() {
        if (m_msgWnd) {
            HWND hwnd = m_msgWnd.get();
            LogMsg("Cleanup: requesting destroy of message window %p", hwnd);
            // DestroyWindow must run on the thread that created the window
            // (the UI thread SetSiteImpl ran on), not this worker thread -
            // dispatch it there via the same SendMessage mechanism already
            // used for bounds collection. A bounded timeout (rather than a
            // bare blocking SendMessage) means a hung/gone UI thread cannot
            // keep this worker thread - and therefore this whole cleanup -
            // from ever completing.
            DWORD_PTR result = 0;
            LRESULT dispatched = SendMessageTimeoutW(hwnd, WM_TAP_DESTROY, 0, 0,
                                                      SMTO_ABORTIFHUNG, 2000, &result);
            if (dispatched == 0) {
                LogMsg("Cleanup: SendMessageTimeout for destroy failed/timed out, error=%lu",
                       GetLastError());
            } else if (IsWindow(hwnd)) {
                LogMsg("Cleanup: message window still alive after destroy request");
            } else {
                LogMsg("Cleanup: message window destroyed");
                // Already destroyed on the correct thread above; release
                // ownership so wil::unique_hwnd's destructor does not also
                // attempt DestroyWindow (which would run on THIS thread,
                // the wrong one, and on an already-invalid handle).
                (void)m_msgWnd.release();
            }
        }
        UnregisterClassW(L"LvtTapMsg", GetCurrentModuleHandle());
        m_vts.reset();
        m_diag.reset();
        m_pipe.reset();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) override {
        if (!m_site) { *ppvSite = nullptr; return E_FAIL; }
        return m_site->QueryInterface(riid, ppvSite);
    }

    // IVisualTreeServiceCallback
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) override
    {
        const bool isAdd = (mutationType == VisualMutationType::Add);
        const bool isRemove = (mutationType == VisualMutationType::Remove);
        std::wstring type, name;
        {
            std::lock_guard<std::mutex> lock(m_nodesMutex);
            if (isAdd) {
                TreeNode node;
                node.handle = element.Handle;
                node.type = element.Type ? element.Type : L"";
                node.name = element.Name ? element.Name : L"";
                node.numChildren = element.NumChildren;
                node.parent = relation.Parent;
                node.childIndex = relation.ChildIndex;
                type = node.type;
                name = node.name;
                m_nodes[element.Handle] = std::move(node);

                if (relation.Parent != 0) {
                    auto it = m_nodes.find(relation.Parent);
                    if (it != m_nodes.end()) {
                        auto& children = it->second.childHandles;
                        children.erase(
                            std::remove(children.begin(), children.end(), element.Handle),
                            children.end());
                        const auto index = std::min<size_t>(
                            relation.ChildIndex, children.size());
                        children.insert(children.begin() + index, element.Handle);
                    }
                } else {
                    m_roots.erase(
                        std::remove(m_roots.begin(), m_roots.end(), element.Handle),
                        m_roots.end());
                    const auto index = std::min<size_t>(
                        relation.ChildIndex, m_roots.size());
                    m_roots.insert(m_roots.begin() + index, element.Handle);
                }
            } else if (isRemove) {
                // Essential for a persistent connection, not optional: the
                // old one-shot-per-tick design never needed this branch at
                // all - a removed element simply would not appear in the
                // *next fresh* replay, since m_nodes was rebuilt from
                // scratch every time. A long-lived connection's m_nodes is
                // never rebuilt, so without this, every element the target
                // ever destroys would stay in the reported tree forever.
                auto it = m_nodes.find(element.Handle);
                if (it != m_nodes.end()) {
                    InstanceHandle parent = it->second.parent;
                    m_nodes.erase(it);
                    if (parent != 0) {
                        auto pit = m_nodes.find(parent);
                        if (pit != m_nodes.end()) {
                            auto& kids = pit->second.childHandles;
                            kids.erase(std::remove(kids.begin(), kids.end(), element.Handle), kids.end());
                        }
                    } else {
                        m_roots.erase(std::remove(m_roots.begin(), m_roots.end(), element.Handle), m_roots.end());
                    }
                }
            }
        }

        // Queue outside m_nodesMutex. The target callback remains memory-only;
        // the command-loop thread serializes and drains events for GET_TREE or
        // POLL_EVENTS.
        if (isAdd || isRemove)
            QueueChangeEvent(
                isAdd, element.Handle, relation.Parent, relation.ChildIndex,
                type, name);
        return S_OK;
    }

    // IVisualTreeServiceCallback2
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle, VisualElementState, LPCWSTR) override
    {
        return S_OK;
    }

private:
    // Parse "x,y,z" or "<x, y, z>" formatted offset string
    static bool ParseOffset(const std::wstring& val, double& x, double& y) {
        // Try "x,y,z" or "<x, y, z>" format
        const wchar_t* p = val.c_str();
        while (*p && (*p == L'<' || *p == L' ')) p++;
        wchar_t* end = nullptr;
        x = wcstod(p, &end);
        if (end == p) return false;
        p = end;
        while (*p && (*p == L',' || *p == L' ')) p++;
        y = wcstod(p, &end);
        return end != p;
    }

    // Collect bounds for a single node — isolated for SEH compatibility
    static void CollectBoundsForNode(IVisualTreeService* vts,
                                     TreeNode& node, InstanceHandle handle,
                                     bool logDetail) {
        if (logDetail) LogMsg("  CollectBoundsForNode ENTER handle=%llu", (unsigned long long)handle);
        unsigned int srcCount = 0, propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* props = nullptr;
        HRESULT hr = vts->GetPropertyValuesChain(
            handle, &srcCount, &sources, &propCount, &props);
        if (FAILED(hr)) {
            return;
        }
        wil::unique_cotaskmem propsMem(props);
        wil::unique_cotaskmem sourcesMem(sources);
        bool hasWidth = false, hasHeight = false;
        bool foundOffset = false;
        for (unsigned int i = 0; i < propCount; i++) {
            wil::unique_bstr type(props[i].Type);
            wil::unique_bstr declaringType(props[i].DeclaringType);
            wil::unique_bstr valueTypeBstr(props[i].ValueType);
            wil::unique_bstr itemType(props[i].ItemType);
            wil::unique_bstr propertyName(props[i].PropertyName);
            wil::unique_bstr propertyValue(props[i].Value);
            props[i].Type = nullptr;
            props[i].DeclaringType = nullptr;
            props[i].ValueType = nullptr;
            props[i].ItemType = nullptr;
            props[i].PropertyName = nullptr;
            props[i].Value = nullptr;

            std::wstring name = propertyName ? propertyName.get() : L"";
            std::wstring value = propertyValue ? propertyValue.get() : L"";
            if (name == L"ActualWidth" && !value.empty()) {
                double v = _wtof(value.c_str());
                if (std::isfinite(v)) {
                    node.width = v;
                    hasWidth = true;
                }
            } else if (name == L"ActualHeight" && !value.empty()) {
                double v = _wtof(value.c_str());
                if (std::isfinite(v)) {
                    node.height = v;
                    hasHeight = true;
                }
            } else if (name == L"ActualOffset" && !value.empty()) {
                double ox = 0, oy = 0;
                if (ParseOffset(value, ox, oy) && std::isfinite(ox) && std::isfinite(oy)) {
                    node.offsetX = ox;
                    node.offsetY = oy;
                    foundOffset = true;
                }
            }
            // Extract important XAML properties for the tree dump.
            // Only capture the first occurrence of each (most-specific in the chain).
            // The capture decision (which properties, and which values count as
            // real data vs. absence) lives in xaml_property_filter.h, where it is
            // unit tested; see that header for why "0" is not an unset sentinel and
            // why looksLikeHandle only applies when ValueType is not confirmed String.
            std::wstring valueType = valueTypeBstr ? valueTypeBstr.get() : L"";
            if (lvt::xaml_should_capture_property(name, value, valueType)) {
                // Only store if not already present (first = most-specific)
                bool found = false;
                for (auto& p : node.properties) {
                    if (p.first == name) { found = true; break; }
                }
                if (!found) {
                    node.properties.emplace_back(name, value);
                }
            }
        }
        for (unsigned int i = 0; i < srcCount; i++) {
            wil::unique_bstr targetType(sources[i].TargetType);
            wil::unique_bstr sourceName(sources[i].Name);
            wil::unique_bstr sourceFile(sources[i].SrcInfo.FileName);
            wil::unique_bstr sourceHash(sources[i].SrcInfo.Hash);
            sources[i].TargetType = nullptr;
            sources[i].Name = nullptr;
            sources[i].SrcInfo.FileName = nullptr;
            sources[i].SrcInfo.Hash = nullptr;
        }
        node.hasBounds = hasWidth && hasHeight;
    }

    // SEH wrapper for single-node bounds collection (cannot use __try with C++ objects)
    static int CollectBoundsForNodeSEH(IVisualTreeService* vts,
                                       TreeNode& node, InstanceHandle handle,
                                       bool logDetail) {
        __try {
            CollectBoundsForNode(vts, node, handle, logDetail);
            return 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    // Every "lvt watch" tick re-injects and walks the *entire* tree from
    // scratch (see run_watch_loop in main.cpp), and this loop runs on the
    // target's UI thread via a blocking SendMessage (LvtTapMsgWndProc).
    //
    // An earlier version of this code capped how long a single pass could
    // run (kUiThreadBudgetMs), on the theory that an unbounded walk over a
    // rich tree could occupy the UI thread long enough to make unrelated
    // work on that thread look hung. That budget was a mistake: measured
    // live against a real, large WinUI3 app (Microsoft Store, ~1100 nodes,
    // ~4ms/node), the wall-clock cutoff meant a different, non-deterministic
    // subset of nodes got bounds each tick — not because the UI changed, but
    // because ordinary timing jitter shifted exactly how many nodes fit in
    // the budget window. Every unaffected element's bounds/properties then
    // flip-flopped between "known" and "absent" every tick, forever, which
    // `watch`'s diffing correctly (and unhelpfully) reported as constant
    // "changed" events — flooding stdout (250MB+ observed in minutes) and
    // burning CPU on serialization for output nobody asked for, which looked
    // to a client (the lvt Viewer) exactly like a connection that was
    // stuck, not one that was overcorrecting on interpreting real data.
    //
    // What still needed fixing after removing that budget: even at full,
    // uninterrupted speed, a single SendMessage-dispatched pass over a rich
    // tree (several seconds for 1000+ nodes) occupies the target's UI thread
    // start to finish with no chance to service its own pending messages in
    // between — which is exactly what a modal window-move loop (DefWindowProc
    // handling WM_NCLBUTTONDOWN/SC_MOVE) *also* needs that same thread for,
    // observed live as the target app feeling laggy/stuttery to drag while
    // `watch` was attached. AdviseThreadProcImpl now dispatches this in small
    // chunks (kBatchSize nodes per SendMessage call) with a short Sleep
    // between chunks, so the target's UI thread gets to drain its own
    // message queue between chunks instead of being monopolized for the
    // whole pass — full correctness is unaffected (every node still gets
    // collected, in the same order, every tick; only the dispatch is
    // chunked), so this does not reintroduce the flapping the time budget
    // caused.
    //
    // The protection against a pathologically slow *overall* collection is
    // still one layer up: xaml_diag_common.cpp's TAP DLL only calls
    // CreateFileW to connect to lvt.exe's pipe *after*
    // CollectBounds/CollectPositionsAndText/SerializeAndSend all finish (see
    // SerializeAndSend below), so lvt.exe's own "TAP DLL did not connect"
    // timeout on the other end of that pipe already bounds the combined
    // cost of this walk, chunked or not, and fails the whole tick cleanly
    // (no partial data) rather than partially collecting.
    //
    // That timeout used to be 15 seconds, which was not a safety margin —
    // it was the actual cause of real, reproducible tree data loss. Traced
    // live against Microsoft Store's animated home page (~1936 elements):
    // a single *successful* collection (every call below returned success)
    // measured 40.8 seconds end to end, because chunking here specifically
    // lets a busy/animating target's UI thread interleave its own work
    // between chunks rather than being monopolized — exactly what an
    // actively animating tree needs a lot of. At 15 seconds, lvt.exe
    // routinely gave up and closed the pipe while this was still
    // legitimately working, so the "fails cleanly" path above was firing
    // for collections that would have succeeded if just given more time.
    // See xaml_diag_common.cpp's kXamlCollectionTimeoutMs (now 60s) for
    // where this is actually bounded today.
    void CollectBounds(IVisualTreeService* vts, size_t start, size_t count) {
        // Fast mode skips GetPropertyValuesChain entirely — the dominant
        // per-node cost (~4.5ms/element, measured live against Microsoft
        // Store and Calculator) of walking an element's *entire* property
        // inheritance chain just to read ActualWidth/ActualHeight out of it.
        // CollectPositionsAndText gets bounds a cheaper way instead (direct
        // FrameworkElement.ActualWidth/ActualHeight via the same IInspectable
        // it already fetches for position/text), so there is nothing for
        // this function to do in fast mode.
        if (m_fastMode) {
            LogMsg("CollectBounds: skipped in fast mode, batch [%zu,%zu)", start,
                   std::min(start + count, m_orderedHandles.size()));
            return;
        }
        size_t end = std::min(start + count, m_orderedHandles.size());
        int collected = 0;
        for (size_t i = start; i < end; i++) {
            InstanceHandle handle = m_orderedHandles[i];
            std::lock_guard<std::mutex> lock(m_nodesMutex);
            auto it = m_nodes.find(handle);
            if (it == m_nodes.end()) continue;
            TreeNode& node = it->second;
            bool logDetail = false;
            int code = CollectBoundsForNodeSEH(vts, node, handle, logDetail);
            if (code != 0) {
                LogMsg("GetPropertyValuesChain crashed for handle %llu: 0x%08X",
                       (unsigned long long)handle, code);
            }
            if (node.hasBounds) collected++;
        }
        LogMsg("CollectBounds: collected bounds for %d/%zu nodes in batch [%zu,%zu)",
               collected, end - start, start, end);
    }

#if LVT_HAS_XAML_PROJECTION
    // Use TransformToVisual to get each element's position relative to the XAML island root.
    // Also reads Text property from TextBlock elements.
    // Tries both WinUI3 (Microsoft.UI.Xaml) and system XAML (Windows.UI.Xaml) interfaces.
    // Chunked the same way, and for the same reason, as CollectBounds above.
    // Unboxes a Content/Header-style IInspectable to a string only when it is
    // actually one — most ContentControls hold a nested UIElement subtree
    // there instead (a StackPanel with an Image+TextBlock, say), and that has
    // no meaningful flat string to show. IPropertyValue is how WinRT
    // represents a boxed primitive regardless of which XAML projection
    // produced it, so this one helper covers both WUX and Microsoft.UI.Xaml
    // without a separate branch for each.
    static bool TryUnboxString(const winrt::Windows::Foundation::IInspectable& value,
                               winrt::hstring& out) {
        if (auto propValue = value.try_as<winrt::Windows::Foundation::IPropertyValue>()) {
            if (propValue.Type() == winrt::Windows::Foundation::PropertyType::String) {
                out = propValue.GetString();
                return !out.empty();
            }
        }
        return false;
    }

    static void SetCollectedProperty(TreeNode& node, const wchar_t* name,
                                     const winrt::hstring& value) {
        if (value.empty())
            return;
        auto existing = std::find_if(
            node.properties.begin(), node.properties.end(),
            [name](const auto& property) { return property.first == name; });
        if (existing != node.properties.end())
            existing->second = std::wstring(value);
        else
            node.properties.emplace_back(name, std::wstring(value));
    }

    void CollectPositionsAndText(size_t start, size_t count) {
        namespace WUX = winrt::Windows::UI::Xaml;
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;

        if (!m_diag) return;

        size_t end = std::min(start + count, m_orderedHandles.size());
        int positioned = 0, textsRead = 0, boundsFromFastPath = 0;
        for (size_t i = start; i < end; i++) {
            InstanceHandle handle = m_orderedHandles[i];
            std::lock_guard<std::mutex> lock(m_nodesMutex);
            auto it = m_nodes.find(handle);
            if (it == m_nodes.end()) continue;
            TreeNode& node = it->second;

            // Keep raw to preserve XAML diagnostics' existing ABI lifetime behavior.
            ::IInspectable* raw = nullptr;
            HRESULT hr = m_diag->GetIInspectableFromHandle(handle, &raw);
            if (FAILED(hr) || !raw) continue;

            try {
                winrt::Windows::Foundation::IInspectable inspectable;
                winrt::copy_from_abi(inspectable, raw);
                raw = nullptr; // ownership transferred

                // Fast mode never ran GetPropertyValuesChain (see
                // CollectBounds), so ActualWidth/ActualHeight have not been
                // read yet — get them the same cheap way position/text
                // already come from: a direct WinRT property read on the
                // IInspectable this loop obtained anyway, no COM property-
                // chain walk. Non-fast mode already has hasBounds from
                // CollectBounds and skips this — it is not wrong to redo it,
                // just pointless cost this path exists specifically to avoid.
                if (m_fastMode && !node.hasBounds) {
                    bool gotBounds = false;
#if LVT_HAS_WINUI3_PROJECTION
                    if (auto fe = inspectable.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
                        double w = fe.ActualWidth(), h = fe.ActualHeight();
                        if (std::isfinite(w) && std::isfinite(h)) {
                            node.width = w;
                            node.height = h;
                            gotBounds = true;
                        }
                    }
#endif
                    if (!gotBounds) {
                        if (auto fe = inspectable.try_as<WUX::FrameworkElement>()) {
                            double w = fe.ActualWidth(), h = fe.ActualHeight();
                            if (std::isfinite(w) && std::isfinite(h)) {
                                node.width = w;
                                node.height = h;
                                gotBounds = true;
                            }
                        }
                    }
                    node.hasBounds = gotBounds;
                    if (gotBounds) boundsFromFastPath++;
                }
                if (!node.hasBounds) continue;

                // Position via TransformToVisual — try WinUI3 first, then system XAML
                bool gotPosition = false;
#if LVT_HAS_WINUI3_PROJECTION
                if (auto uiElem = inspectable.try_as<winrt::Microsoft::UI::Xaml::UIElement>()) {
                    auto pt = uiElem.TransformToVisual(nullptr).TransformPoint({0, 0});
                    if (std::isfinite(pt.X) && std::isfinite(pt.Y)) {
                        node.offsetX = static_cast<double>(pt.X);
                        node.offsetY = static_cast<double>(pt.Y);
                        gotPosition = true;
                    }
                }
#endif
                if (!gotPosition) {
                    if (auto uiElem = inspectable.try_as<WUX::UIElement>()) {
                        auto pt = uiElem.TransformToVisual(nullptr).TransformPoint({0, 0});
                        if (std::isfinite(pt.X) && std::isfinite(pt.Y)) {
                            node.offsetX = static_cast<double>(pt.X);
                            node.offsetY = static_cast<double>(pt.Y);
                            gotPosition = true;
                        }
                    }
                }
                if (gotPosition) positioned++;

                // Text from TextBlock — try WinUI3 first, then system XAML
                winrt::hstring text;
#if LVT_HAS_WINUI3_PROJECTION
                if (auto tb = inspectable.try_as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>())
                    text = tb.Text();
#endif
                if (text.empty()) {
                    if (auto tb = inspectable.try_as<WUXC::TextBlock>())
                        text = tb.Text();
                }
                if (!text.empty()) {
                    SetCollectedProperty(node, L"Text", text);
                    textsRead++;
                }

                // Content from ContentControl (Button, ListViewItem, ...) —
                // only when it unboxes to a plain string (see TryUnboxString):
                // most controls' Content is a nested element subtree, which
                // has nothing flat to show here. This runs in both modes —
                // GetPropertyValuesChain's own filter (xaml_property_filter.h)
                // treats a reference-typed Content as a handle and drops it,
                // so today this is new data even in the default/full path,
                // not a duplicate of what GetPropertyValuesChain already
                // reports.
                winrt::hstring content;
#if LVT_HAS_WINUI3_PROJECTION
                if (auto cc = inspectable.try_as<winrt::Microsoft::UI::Xaml::Controls::ContentControl>())
                    TryUnboxString(cc.Content(), content);
#endif
                if (content.empty()) {
                    if (auto cc = inspectable.try_as<WUXC::ContentControl>())
                        TryUnboxString(cc.Content(), content);
                }
                if (!content.empty())
                    SetCollectedProperty(node, L"Content", content);
            } catch (...) {
                // Swallow WinRT exceptions — element may be in an invalid state
            }

            if (raw) raw->Release();
        }
        LogMsg("CollectPositionsAndText: %d positioned, %d texts, "
               "%d bounds-from-fast-path in batch [%zu,%zu)",
               positioned, textsRead, boundsFromFastPath, start, end);
    }
#endif

    // Called on the UI thread via SendMessage from the worker thread
public:
    void CollectBoundsOnUIThread(size_t start, size_t count) {
        CollectBounds(m_vts.get(), start, count);
    }
#if LVT_HAS_XAML_PROJECTION
    void CollectPositionsOnUIThread(size_t start, size_t count) {
        CollectPositionsAndText(start, count);
    }
#endif
private:

    static std::wstring Escape(const std::wstring& s) {
        std::wstring r;
        r.reserve(s.size());
        for (wchar_t c : s) {
            if (c == L'"') { r += L"\\\""; }
            else if (c == L'\\') { r += L"\\\\"; }
            else if (c < 0x20) {
                // Escape all control characters as \uXXXX
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04X", (unsigned)c);
                r += buf;
            }
            else { r += c; }
        }
        return r;
    }

    std::wstring SerializeNode(InstanceHandle handle) {
        auto it = m_nodes.find(handle);
        if (it == m_nodes.end()) return L"null";
        auto& n = it->second;

        std::wstring j = L"{\"type\":\"" + Escape(n.type) + L"\"";
        if (!n.name.empty())
            j += L",\"name\":\"" + Escape(n.name) + L"\"";
        j += L",\"handle\":" + std::to_wstring(n.handle);

        if (n.hasBounds) {
            // Always include width/height for tree dump consumers.
            // Only include offsetX/offsetY when non-zero — WinUI3's XAML diagnostics
            // serializes ActualOffset (Vector3) as "0", so zero offsets are unreliable.
            char buf[128];
            snprintf(buf, sizeof(buf), ",\"width\":%.1f,\"height\":%.1f",
                     n.width, n.height);
            for (const char* p = buf; *p; p++) j += static_cast<wchar_t>(*p);
            if (n.offsetX != 0.0 || n.offsetY != 0.0) {
                snprintf(buf, sizeof(buf), ",\"offsetX\":%.1f,\"offsetY\":%.1f",
                         n.offsetX, n.offsetY);
                for (const char* p = buf; *p; p++) j += static_cast<wchar_t>(*p);
            }
        }

        // Serialize extracted properties
        if (!n.properties.empty()) {
            j += L",\"properties\":{";
            for (size_t i = 0; i < n.properties.size(); i++) {
                if (i) j += L",";
                j += L"\"" + Escape(n.properties[i].first) + L"\":\"" + Escape(n.properties[i].second) + L"\"";
            }
            j += L"}";
        }

        if (!n.childHandles.empty()) {
            j += L",\"children\":[";
            for (size_t i = 0; i < n.childHandles.size(); i++) {
                if (i) j += L",";
                j += SerializeNode(n.childHandles[i]);
            }
            j += L"]";
        }
        j += L"}";
        return j;
    }

    void SerializeAndSend() {
        // One lock for the whole recursive walk (SerializeNode is
        // non-reentrant with respect to m_nodesMutex - it is only ever
        // called from here) so the tree serialized is a single consistent
        // snapshot, not a mix of before/after some concurrent Add/Remove
        // that happened to land mid-walk. Released before the pipe write
        // below, which can block on a slow/busy reader and has nothing to
        // do with m_nodes.
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(m_nodesMutex);
            LogMsg("SerializeAndSend: nodes=%zu, roots=%zu", m_nodes.size(), m_roots.size());

            // Every GET_TREE request gets exactly one response line, even
            // an empty "[]" - lvt.exe's connection object is blocked
            // waiting for a reply (see xaml_diag_common.cpp's get_tree()),
            // and silently returning here without writing anything would
            // hang it until its own read timeout instead of completing
            // quickly with "no data".
            json = L"[";
            for (size_t i = 0; i < m_roots.size(); i++) {
                if (i) json += L",";
                json += SerializeNode(m_roots[i]);
            }
            json += L"]";
        }
        LogMsg("SerializeAndSend: built JSON, %zu wchars", json.size());

        int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                            utf8.data(), len, nullptr, nullptr);

        if (WriteLine(utf8)) {
            LogMsg("SerializeAndSend: wrote %d bytes to the persistent pipe", len);
        } else {
            LogMsg("SerializeAndSend: failed to write to pipe, error=%lu", GetLastError());
        }
    }
};

// Window procedure for dispatching GetPropertyValuesChain to UI thread
static LRESULT CALLBACK LvtTapMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == LvtTap::WM_COLLECT_BOUNDS) {
        auto* req = reinterpret_cast<BatchRequest*>(lParam);
        if (req && req->self) {
            req->self->CollectBoundsOnUIThread(req->start, req->count);
        }
        return 0;
    }
    if (msg == LvtTap::WM_COLLECT_BOUNDS + 1) {
#if LVT_HAS_XAML_PROJECTION
        auto* req = reinterpret_cast<BatchRequest*>(lParam);
        if (req && req->self) {
            req->self->CollectPositionsOnUIThread(req->start, req->count);
        }
#endif
        return 0;
    }
    if (msg == LvtTap::WM_TAP_DESTROY) {
        // Runs on the thread that created hwnd (this window's owning UI
        // thread) - see CleanupUIResources for why DestroyWindow cannot be
        // called directly from the worker thread instead.
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == LvtTap::WM_PROPERTY_COMMAND) {
        auto* command = reinterpret_cast<TapPropertyCommand*>(lParam);
        if (command) {
            command->hresult = E_FAIL;
            command->error = L"Property command did not complete";
            command->properties.clear();
            command->hasReadback = false;
            command->readback = {};
            // ExecutePropertyCommand is called only here, on the message
            // window's owning XAML UI thread.
            auto* self = reinterpret_cast<LvtTap*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self)
                self->ExecutePropertyCommand(*command);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// COM class factory
class LvtTapFactory : public IClassFactory {
    LONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&m_refCount);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override {
        if (pOuter) return CLASS_E_NOAGGREGATION;
        wil::com_ptr<LvtTap> tap;
        tap.attach(new (std::nothrow) LvtTap());
        if (!tap) return E_OUTOFMEMORY;
        return tap->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

extern "C" {

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    LogMsg("DllGetClassObject called");
    if (rclsid != CLSID_LvtTap) return CLASS_E_CLASSNOTAVAILABLE;
    wil::com_ptr<LvtTapFactory> factory;
    factory.attach(new (std::nothrow) LvtTapFactory());
    if (!factory) return E_OUTOFMEMORY;
    return factory->QueryInterface(riid, ppv);
}

// Always returns S_FALSE: this module cannot be safely unloaded mid-session.
//
// InitializeXamlDiagnosticsEx calls LoadLibraryEx on this DLL (refcount=1),
// then creates an LvtTap object via DllGetClassObject and calls SetSite on it.
// The runtime retains the resulting IObjectWithSite* COM pointer — whose vtable
// points into this module — for the entire lifetime of the diagnostics session.
// No public API (IXamlDiagnostics, IVisualTreeService, or the free functions in
// xamlOM.h) provides a way to force the runtime to release that reference.
//
// After DISCONNECT teardown (CleanupUIResources), the runtime still holds the
// IObjectWithSite* and will call SetSite(newSite) on it if the same endpoint is
// reused for a subsequent lvt connection. Calling FreeLibrary here would unmap
// this code segment, leaving the runtime's pointer dangling and crashing the
// target on the next vtable call.
//
// See docs/tap-dll-design.md § "Module lifetime and why DllCanUnloadNow returns
// S_FALSE" for the full evidence chain and the specific runtime API change that
// would be needed to make safe unload possible.
HRESULT STDAPICALLTYPE DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        LogMsg("DllMain: DLL_PROCESS_ATTACH");
    }
    return TRUE;
}

} // extern "C"
