// lvt_tap.cpp — Tool Attachment Point DLL for XAML diagnostics
// This DLL gets injected into the target process by InitializeXamlDiagnosticsEx.
// It implements IObjectWithSite to receive IXamlDiagnostics, then walks the
// XAML visual tree via IVisualTreeService and sends results back over a named pipe.

#include <Windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <string>
#include <map>
#include <vector>

// Define GUIDs that xamlOM.h only forward-declares
// (no _i.c file or .lib provides these definitions)
// IVisualTreeServiceCallback: {AA7A8931-80E4-4FEC-8F3B-553F87B4966E}
const IID IID_IVisualTreeServiceCallback =
    { 0xAA7A8931, 0x80E4, 0x4FEC, { 0x8F, 0x3B, 0x55, 0x3F, 0x87, 0xB4, 0x96, 0x6E } };
// IVisualTreeServiceCallback2: {BAD9EB88-AE77-4397-B948-5FA2DB0A19EA}
const IID IID_IVisualTreeServiceCallback2 =
    { 0xBAD9EB88, 0xAE77, 0x4397, { 0xB9, 0x48, 0x5F, 0xA2, 0xDB, 0x0A, 0x19, 0xEA } };

// {B8F3E2D1-9A4C-4F5E-B6D7-8C1A3E5F7D9B}
static const CLSID CLSID_LvtTap =
    { 0xB8F3E2D1, 0x9A4C, 0x4F5E, { 0xB6, 0xD7, 0x8C, 0x1A, 0x3E, 0x5F, 0x7D, 0x9B } };

struct TreeNode {
    InstanceHandle handle = 0;
    std::wstring type;
    std::wstring name;
    unsigned int numChildren = 0;
    InstanceHandle parent = 0;
    unsigned int childIndex = 0;
    std::vector<InstanceHandle> childHandles;
};

class LvtTap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
    LONG m_refCount = 1;
    IUnknown* m_site = nullptr;
    IVisualTreeService* m_vts = nullptr;
    std::map<InstanceHandle, TreeNode> m_nodes;
    std::vector<InstanceHandle> m_roots;
    std::wstring m_pipeName;

public:
    // IUnknown
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

    // IObjectWithSite
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pSite) override {
        if (m_site) { m_site->Release(); m_site = nullptr; }
        if (m_vts) { m_vts->Release(); m_vts = nullptr; }

        m_site = pSite;
        if (m_site) m_site->AddRef();

        if (!pSite) return S_OK;

        __try {
            return SetSiteImpl(pSite);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return E_FAIL;
        }
    }

    HRESULT SetSiteImpl(IUnknown* pSite) {
        // Get IXamlDiagnostics from the site
        IXamlDiagnostics* diag = nullptr;
        HRESULT hr = pSite->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&diag);
        if (FAILED(hr) || !diag) return S_OK;

        // Retrieve pipe name from initialization data
        BSTR initData = nullptr;
        diag->GetInitializationData(&initData);
        if (initData) {
            m_pipeName = initData;
            SysFreeString(initData);
        }

        // Get IVisualTreeService
        hr = diag->QueryInterface(__uuidof(IVisualTreeService), (void**)&m_vts);
        diag->Release();
        if (FAILED(hr) || !m_vts) return S_OK;

        // Calling AdviseVisualTreeChange from SetSite thread can hang.
        // Use a worker thread (same approach as Windhawk / microsoft-ui-xaml).
        AddRef();
        HANDLE hThread = CreateThread(nullptr, 0, &AdviseThreadProc, this, 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, 10000);
            CloseHandle(hThread);
        } else {
            Release();
        }

        return S_OK;
    }

    static DWORD WINAPI AdviseThreadProc(LPVOID param) {
        auto* self = reinterpret_cast<LvtTap*>(param);
        __try {
            IVisualTreeServiceCallback* cb =
                static_cast<IVisualTreeServiceCallback*>(
                    static_cast<IVisualTreeServiceCallback2*>(self));

            HRESULT hr = self->m_vts->AdviseVisualTreeChange(cb);
            if (SUCCEEDED(hr)) {
                // Give the XAML runtime time to dispatch initial tree nodes
                Sleep(500);
                self->SerializeAndSend();
                self->m_vts->UnadviseVisualTreeChange(cb);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Silently handle crash — don't bring down the target process
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
    static std::wstring Escape(const std::wstring& s) {
        std::wstring r;
        r.reserve(s.size());
        for (wchar_t c : s) {
            switch (c) {
            case L'"':  r += L"\\\""; break;
            case L'\\': r += L"\\\\"; break;
            case L'\n': r += L"\\n";  break;
            case L'\r': r += L"\\r";  break;
            case L'\t': r += L"\\t";  break;
            default:    r += c;
            }
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

        // Note: GetPropertyValuesChain is not called here because it requires
        // the same apartment as the XAML runtime (STA). Our worker thread can't
        // safely call it. The type and name from OnVisualTreeChange are sufficient.

        // Children
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
        if (m_pipeName.empty() || m_nodes.empty()) return;

        // Build JSON array of root elements
        std::wstring json = L"[";
        for (size_t i = 0; i < m_roots.size(); i++) {
            if (i) json += L",";
            json += SerializeNode(m_roots[i]);
        }
        json += L"]";

        // Convert to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                            utf8.data(), len, nullptr, nullptr);

        // Write to named pipe
        HANDLE pipe = CreateFileW(m_pipeName.c_str(), GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(pipe, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
        }
    }

    ~LvtTap() {
        if (m_vts) m_vts->Release();
        if (m_site) m_site->Release();
    }
};

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

// DLL exports
extern "C" {

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_LvtTap) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) LvtTapFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT STDAPICALLTYPE DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

} // extern "C"
