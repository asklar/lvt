// lvt_tap.cpp — TAP DLL for XAML diagnostics
// Injected into the target process by InitializeXamlDiagnosticsEx.
// Implements IObjectWithSite → receives IXamlDiagnostics → walks XAML tree
// via IVisualTreeService::AdviseVisualTreeChange → sends JSON over named pipe.

#include <Windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <cmath>
#include <unknwn.h>

#include "xaml_property_filter.h"

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
    fprintf(logFile, "[%lu] ", GetCurrentThreadId());
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

// Forward declaration for WndProc
static LRESULT CALLBACK LvtTapMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

class LvtTap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
    LONG m_refCount = 1;
    wil::com_ptr<IUnknown> m_site;
    wil::com_ptr<IXamlDiagnostics> m_diag;
    wil::unique_hwnd m_msgWnd; // Message-only window for UI thread dispatch
    std::map<InstanceHandle, TreeNode> m_nodes;
    std::vector<InstanceHandle> m_roots;
    std::wstring m_pipeName;
    bool m_collectProps = false;

public:
    wil::com_ptr<IVisualTreeService> m_vts;
    static constexpr UINT WM_COLLECT_BOUNDS = WM_USER + 100;

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
            // Format: "pipe_name" or "pipe_name|PROPS"
            auto sep = data.find(L'|');
            if (sep != std::wstring::npos) {
                m_pipeName = data.substr(0, sep);
                std::wstring flags = data.substr(sep + 1);
                m_collectProps = (flags.find(L"PROPS") != std::wstring::npos);
            } else {
                m_pipeName = data;
            }
            LogMsg("Pipe name: %ls, collectProps: %d", m_pipeName.c_str(), m_collectProps);
        }

        hr = diag->QueryInterface(IID_PPV_ARGS(m_vts.put()));
        if (FAILED(hr) || !m_vts) {
            LogMsg("QI for IVisualTreeService failed: 0x%08X", hr);
            return S_OK;
        }

        m_diag = std::move(diag);

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

    DWORD AdviseThreadProcImpl() {
        LogMsg("AdviseThread starting");

        __try {
            IVisualTreeServiceCallback* cb =
                static_cast<IVisualTreeServiceCallback*>(
                    static_cast<IVisualTreeServiceCallback2*>(this));

            HRESULT hr = m_vts->AdviseVisualTreeChange(cb);
            LogMsg("AdviseVisualTreeChange returned 0x%08X, nodes=%zu, roots=%zu",
                   hr, m_nodes.size(), m_roots.size());

            if (SUCCEEDED(hr)) {
                if (m_nodes.empty()) {
                    Sleep(500);
                    LogMsg("After sleep: nodes=%zu", m_nodes.size());
                }
                // Dispatch GetPropertyValuesChain to UI thread via message window.
                // SendMessage blocks until the UI thread processes the message.
                if (m_msgWnd) {
                    LogMsg("Dispatching CollectBounds to UI thread via SendMessage");
                    SendMessageW(m_msgWnd.get(), WM_COLLECT_BOUNDS, 0,
                                 reinterpret_cast<LPARAM>(this));
                }
                // Get element positions via TransformToVisual (works around broken
                // ActualOffset serialization in WinUI3). Must run on the UI thread.
#if LVT_HAS_XAML_PROJECTION
                if (m_msgWnd) {
                    SendMessageW(m_msgWnd.get(), WM_COLLECT_BOUNDS + 1, 0,
                                 reinterpret_cast<LPARAM>(this));
                }
#endif
                SerializeAndSend();
                m_vts->UnadviseVisualTreeChange(cb);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("AdviseThread crashed: 0x%08X", GetExceptionCode());
        }

        return 0;
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
        if (mutationType == VisualMutationType::Add) {
            TreeNode node;
            node.handle = element.Handle;
            node.type = element.Type ? element.Type : L"";
            node.name = element.Name ? element.Name : L"";
            node.numChildren = element.NumChildren;
            node.parent = relation.Parent;
            node.childIndex = relation.ChildIndex;
            m_nodes[element.Handle] = std::move(node);

            if (relation.Parent != 0) {
                auto it = m_nodes.find(relation.Parent);
                if (it != m_nodes.end()) {
                    it->second.childHandles.push_back(element.Handle);
                }
            } else {
                m_roots.push_back(element.Handle);
            }
        }
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
            wil::unique_bstr propertyName(props[i].PropertyName);
            wil::unique_bstr propertyValue(props[i].Value);
            props[i].Type = nullptr;
            props[i].DeclaringType = nullptr;
            props[i].ValueType = nullptr;
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
            sources[i].TargetType = nullptr;
            sources[i].Name = nullptr;
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
    // target's UI thread via a blocking SendMessage (LvtTapMsgWndProc). For a
    // rich production tree (hundreds to thousands of elements — e.g. File
    // Explorer's or Settings' WinUI3 shell) an unbounded walk here can occupy
    // the UI thread for long enough that unrelated, legitimate work on that
    // same thread (e.g. a new top-level window's own startup handshake)
    // appears to hang, repeating every poll interval for as long as `watch`
    // runs. kUiThreadBudgetMs caps how long a single pass may run; any nodes
    // past the budget simply keep whatever bounds they already had (none, on
    // a fresh injection) and are retried on the next tick, same as a node
    // this pass never got to for any other reason.
    //
    // Measured cost per GetPropertyValuesChain call on a real, richly-styled
    // WinUI3 app (Settings, 1104 nodes) is ~4ms — not cheap, so a small
    // budget (200ms, tried first) only covered ~4% of a real app's tree,
    // reintroducing the bounds/property/text gaps this budget exists
    // alongside a fix for. 1500ms is comfortably clear of the ~5s threshold
    // Windows itself uses to decide a window has stopped responding, so a
    // pass at this budget never causes the target to be *reported* as hung,
    // while covering several hundred nodes per tick — enough for most real
    // apps' visible content. Trees larger than that still degrade
    // gracefully (the same later nodes miss every tick, deterministically,
    // rather than intermittently) rather than ever blocking indefinitely.
    static constexpr DWORD64 kUiThreadBudgetMs = 1500;

    void CollectBounds(IVisualTreeService* vts) {
        LogMsg("CollectBounds: collecting layout for %zu nodes on thread %lu",
               m_nodes.size(), GetCurrentThreadId());
        int collected = 0;
        int idx = 0;
        const DWORD64 start = GetTickCount64();
        for (auto& [handle, node] : m_nodes) {
            if (GetTickCount64() - start > kUiThreadBudgetMs) {
                LogMsg("CollectBounds: time budget exceeded after %d/%zu nodes; "
                       "remaining nodes will be retried next tick", idx, m_nodes.size());
                break;
            }
            bool logDetail = false;
            int code = CollectBoundsForNodeSEH(vts, node, handle, logDetail);
            if (code != 0) {
                LogMsg("GetPropertyValuesChain crashed for handle %llu: 0x%08X",
                       (unsigned long long)handle, code);
            }
            if (node.hasBounds) collected++;
            idx++;
        }
        LogMsg("CollectBounds: collected bounds for %d/%zu nodes", collected, m_nodes.size());
    }

#if LVT_HAS_XAML_PROJECTION
    // Use TransformToVisual to get each element's position relative to the XAML island root.
    // Also reads Text property from TextBlock elements.
    // Tries both WinUI3 (Microsoft.UI.Xaml) and system XAML (Windows.UI.Xaml) interfaces.
    void CollectPositionsAndText() {
        namespace WUX = winrt::Windows::UI::Xaml;
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;

        if (!m_diag) return;

        int positioned = 0, textsRead = 0;
        const DWORD64 start = GetTickCount64();
        for (auto& [handle, node] : m_nodes) {
            // Same UI-thread time budget as CollectBounds, and for the same
            // reason: this also runs on the target's UI thread via a blocking
            // SendMessage, once per watch tick, forever.
            if (GetTickCount64() - start > kUiThreadBudgetMs) {
                LogMsg("CollectPositionsAndText: time budget exceeded; "
                       "remaining nodes will be retried next tick");
                break;
            }
            if (!node.hasBounds) continue;

            // Keep raw to preserve XAML diagnostics' existing ABI lifetime behavior.
            ::IInspectable* raw = nullptr;
            HRESULT hr = m_diag->GetIInspectableFromHandle(handle, &raw);
            if (FAILED(hr) || !raw) continue;

            try {
                winrt::Windows::Foundation::IInspectable inspectable;
                winrt::copy_from_abi(inspectable, raw);
                raw = nullptr; // ownership transferred

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
                    node.properties.emplace_back(L"Text", std::wstring(text));
                    textsRead++;
                }
            } catch (...) {
                // Swallow WinRT exceptions — element may be in an invalid state
            }

            if (raw) raw->Release();
        }
        LogMsg("CollectPositionsAndText: %d positioned, %d texts", positioned, textsRead);
    }
#endif

    // Called on the UI thread via SendMessage from the worker thread
public:
    void CollectBoundsOnUIThread() {
        CollectBounds(m_vts.get());
    }
#if LVT_HAS_XAML_PROJECTION
    void CollectPositionsOnUIThread() {
        CollectPositionsAndText();
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
        LogMsg("SerializeAndSend: nodes=%zu, roots=%zu, pipe=%ls",
               m_nodes.size(), m_roots.size(), m_pipeName.c_str());

        if (m_pipeName.empty() || m_nodes.empty()) return;

        std::wstring json = L"[";
        for (size_t i = 0; i < m_roots.size(); i++) {
            if (i) json += L",";
            json += SerializeNode(m_roots[i]);
        }
        json += L"]";

        int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                            utf8.data(), len, nullptr, nullptr);

        wil::unique_hfile pipe(CreateFileW(m_pipeName.c_str(), GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
        if (pipe) {
            DWORD written = 0;
            WriteFile(pipe.get(), utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            FlushFileBuffers(pipe.get());
            LogMsg("Wrote %lu bytes to pipe", written);
        } else {
            LogMsg("Failed to open pipe: %lu", GetLastError());
        }
    }
};

// Window procedure for dispatching GetPropertyValuesChain to UI thread
static LRESULT CALLBACK LvtTapMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == LvtTap::WM_COLLECT_BOUNDS) {
        auto* self = reinterpret_cast<LvtTap*>(lParam);
        if (self) {
            self->CollectBoundsOnUIThread();
        }
        return 0;
    }
    if (msg == LvtTap::WM_COLLECT_BOUNDS + 1) {
#if LVT_HAS_XAML_PROJECTION
        auto* self = reinterpret_cast<LvtTap*>(lParam);
        if (self) {
            self->CollectPositionsOnUIThread();
        }
#endif
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

HRESULT STDAPICALLTYPE DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        LogMsg("DllMain: DLL_PROCESS_ATTACH");
    }
    return TRUE;
}

} // extern "C"
