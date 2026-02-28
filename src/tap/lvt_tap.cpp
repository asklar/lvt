// lvt_tap.cpp — TAP DLL for XAML diagnostics
// Injected into the target process by InitializeXamlDiagnosticsEx.
// Implements IObjectWithSite → receives IXamlDiagnostics → walks XAML tree
// via IVisualTreeService::AdviseVisualTreeChange → sends JSON over named pipe.

#include <Windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <cmath>

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
    IUnknown* m_site = nullptr;
    IXamlDiagnostics* m_diag = nullptr;
    HWND m_msgWnd = nullptr; // Message-only window for UI thread dispatch
    std::map<InstanceHandle, TreeNode> m_nodes;
    std::vector<InstanceHandle> m_roots;
    std::wstring m_pipeName;
    bool m_collectProps = false;

public:
    IVisualTreeService* m_vts = nullptr;
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

        if (m_site) { m_site->Release(); m_site = nullptr; }
        if (m_vts) { m_vts->Release(); m_vts = nullptr; }

        m_site = pSite;
        if (m_site) m_site->AddRef();

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
        IXamlDiagnostics* diag = nullptr;
        HRESULT hr = pSite->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&diag);
        if (FAILED(hr) || !diag) {
            LogMsg("QI for IXamlDiagnostics failed: 0x%08X", hr);
            return S_OK;
        }

        BSTR initData = nullptr;
        diag->GetInitializationData(&initData);
        if (initData) {
            std::wstring data(initData);
            SysFreeString(initData);
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

        hr = diag->QueryInterface(__uuidof(IVisualTreeService), (void**)&m_vts);
        if (FAILED(hr) || !m_vts) {
            LogMsg("QI for IVisualTreeService failed: 0x%08X", hr);
            diag->Release();
            return S_OK;
        }

        m_diag = diag; // Keep reference for GetIInspectableFromHandle

        // Create a message-only window on the UI thread for dispatching
        // GetPropertyValuesChain calls (which have thread affinity).
        WNDCLASSW wc = {};
        wc.lpfnWndProc = LvtTapMsgWndProc;
        wc.hInstance = GetCurrentModuleHandle();
        wc.lpszClassName = L"LvtTapMsg";
        RegisterClassW(&wc);
        m_msgWnd = CreateWindowExW(0, L"LvtTapMsg", nullptr, 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (m_msgWnd) {
            SetWindowLongPtrW(m_msgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            LogMsg("Created message window %p on thread %lu", m_msgWnd, GetCurrentThreadId());
        }

        // AdviseVisualTreeChange hangs if called on the SetSite thread.
        // Fire-and-forget a worker thread (same as Windhawk).
        AddRef();
        HANDLE hThread = CreateThread(nullptr, 0, &AdviseThreadProc, this, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread); // Don't wait — return immediately to unblock UI thread
        } else {
            LogMsg("CreateThread failed: %lu", GetLastError());
            Release();
        }

        return S_OK;
    }

    static DWORD WINAPI AdviseThreadProc(LPVOID param) {
        auto* self = reinterpret_cast<LvtTap*>(param);
        LogMsg("AdviseThread starting");

        __try {
            IVisualTreeServiceCallback* cb =
                static_cast<IVisualTreeServiceCallback*>(
                    static_cast<IVisualTreeServiceCallback2*>(self));

            HRESULT hr = self->m_vts->AdviseVisualTreeChange(cb);
            LogMsg("AdviseVisualTreeChange returned 0x%08X, nodes=%zu, roots=%zu",
                   hr, self->m_nodes.size(), self->m_roots.size());

            if (SUCCEEDED(hr)) {
                if (self->m_nodes.empty()) {
                    Sleep(500);
                    LogMsg("After sleep: nodes=%zu", self->m_nodes.size());
                }
                // Dispatch GetPropertyValuesChain to UI thread via message window.
                // SendMessage blocks until the UI thread processes the message.
                if (self->m_msgWnd) {
                    LogMsg("Dispatching CollectBounds to UI thread via SendMessage");
                    SendMessageW(self->m_msgWnd, WM_COLLECT_BOUNDS, 0,
                                 reinterpret_cast<LPARAM>(self));
                }
                self->SerializeAndSend();
                self->m_vts->UnadviseVisualTreeChange(cb);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LogMsg("AdviseThread crashed: 0x%08X", GetExceptionCode());
        }

        self->Release();
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
    static void CollectBoundsForNode(IVisualTreeService* vts, TreeNode& node, InstanceHandle handle,
                                     bool /*logDetail*/) {
        unsigned int srcCount = 0, propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* props = nullptr;
        HRESULT hr = vts->GetPropertyValuesChain(
            handle, &srcCount, &sources, &propCount, &props);
        if (FAILED(hr)) {
            return;
        }
        bool hasWidth = false, hasHeight = false;
        for (unsigned int i = 0; i < propCount; i++) {
            std::wstring name = props[i].PropertyName ? props[i].PropertyName : L"";
            std::wstring value = props[i].Value ? props[i].Value : L"";
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
                }
            }
            if (props[i].Type) SysFreeString(props[i].Type);
            if (props[i].DeclaringType) SysFreeString(props[i].DeclaringType);
            if (props[i].ValueType) SysFreeString(props[i].ValueType);
            if (props[i].PropertyName) SysFreeString(props[i].PropertyName);
            if (props[i].Value) SysFreeString(props[i].Value);
        }
        for (unsigned int i = 0; i < srcCount; i++) {
            if (sources[i].TargetType) SysFreeString(sources[i].TargetType);
            if (sources[i].Name) SysFreeString(sources[i].Name);
        }
        if (props) CoTaskMemFree(props);
        if (sources) CoTaskMemFree(sources);
        node.hasBounds = hasWidth && hasHeight;
    }

    // SEH wrapper for single-node bounds collection (cannot use __try with C++ objects)
    static int CollectBoundsForNodeSEH(IVisualTreeService* vts, TreeNode& node, InstanceHandle handle,
                                       bool logDetail) {
        __try {
            CollectBoundsForNode(vts, node, handle, logDetail);
            return 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    void CollectBounds(IVisualTreeService* vts) {
        LogMsg("CollectBounds: collecting layout for %zu nodes on thread %lu",
               m_nodes.size(), GetCurrentThreadId());
        int collected = 0;
        int idx = 0;
        for (auto& [handle, node] : m_nodes) {
            bool logDetail = (idx < 3); // Log details for first 3 nodes
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

    // Called on the UI thread via SendMessage from the worker thread
public:
    void CollectBoundsOnUIThread() {
        CollectBounds(m_vts);
    }
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
            // Use snprintf for consistent decimal formatting
            char buf[128];
            snprintf(buf, sizeof(buf), ",\"width\":%.1f,\"height\":%.1f,\"offsetX\":%.1f,\"offsetY\":%.1f",
                     n.width, n.height, n.offsetX, n.offsetY);
            // Convert to wide string
            for (const char* p = buf; *p; p++) j += static_cast<wchar_t>(*p);
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

        HANDLE pipe = CreateFileW(m_pipeName.c_str(), GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(pipe, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
            LogMsg("Wrote %lu bytes to pipe", written);
        } else {
            LogMsg("Failed to open pipe: %lu", GetLastError());
        }
    }

    ~LvtTap() {
        if (m_msgWnd) DestroyWindow(m_msgWnd);
        if (m_vts) m_vts->Release();
        if (m_diag) m_diag->Release();
        if (m_site) m_site->Release();
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
        auto* tap = new (std::nothrow) LvtTap();
        if (!tap) return E_OUTOFMEMORY;
        HRESULT hr = tap->QueryInterface(riid, ppv);
        tap->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

extern "C" {

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    LogMsg("DllGetClassObject called");
    if (rclsid != CLSID_LvtTap) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) LvtTapFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
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
