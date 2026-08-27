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
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <optional>
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
    fprintf(logFile, "[%llu][%lu] ", GetTickCount64(), GetCurrentThreadId());
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
    // Guards every write to m_pipe. A GET_TREE response (written from the
    // worker/command-loop thread) and a pushed CHANGE event (written from
    // whichever thread XAML's OnVisualTreeChange happens to call back on)
    // must never interleave their bytes on the wire.
    std::mutex m_pipeWriteMutex;

public:
    wil::com_ptr<IVisualTreeService> m_vts;
    static constexpr UINT WM_COLLECT_BOUNDS = WM_USER + 100;
    // WM_COLLECT_BOUNDS + 1 is used for CollectPositionsAndText dispatch
    // (see LvtTapMsgWndProc). This one asks the UI thread (the only thread
    // allowed to destroy a window it owns) to destroy m_msgWnd during final
    // cleanup - see CleanupUIResources.
    static constexpr UINT WM_TAP_DESTROY = WM_USER + 102;

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

    // Writes one line (message + '\n') to the pipe. Safe to call from any
    // thread - guarded (see m_pipeWriteMutex) so a pushed CHANGE event can
    // never interleave its bytes with a GET_TREE response.
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

    // Writes one unsolicited {"type":"CHANGE",...} line - see
    // OnVisualTreeChange, which calls this after releasing m_nodesMutex
    // (never while holding it, to keep lock ordering simple: WriteLine only
    // ever needs m_pipeWriteMutex). Safe to call before ServeConnection has
    // run (no pipe yet - SetSiteImpl's synchronous initial replay happens
    // first) or after the connection has ended: WriteLine just fails
    // quietly in both cases, same as for any other caller, and a
    // subsequent GET_TREE response always reflects current reality
    // regardless of whether this push made it out.
    void PushChangeEvent(bool added, InstanceHandle handle, InstanceHandle parent,
                         unsigned int childIndex, const std::wstring& type, const std::wstring& name) {
        std::wstring json = L"{\"type\":\"CHANGE\",\"mutation\":\"";
        json += added ? L"add" : L"remove";
        json += L"\",\"handle\":" + std::to_wstring(handle);
        json += L",\"parent\":" + std::to_wstring(parent);
        if (added) {
            json += L",\"childIndex\":" + std::to_wstring(childIndex);
            json += L",\"elementType\":\"" + Escape(type) + L"\"";
            if (!name.empty())
                json += L",\"name\":\"" + Escape(name) + L"\"";
        }
        json += L"}";

        int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                            utf8.data(), len, nullptr, nullptr);
        bool sent = WriteLine(utf8);
        LogMsg("PushChangeEvent: %s handle=%llu parent=%llu sent=%d",
               added ? "add" : "remove", (unsigned long long)handle, (unsigned long long)parent, sent);
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
            LogMsg("Sent READY, entering command loop");
            RunCommandLoop();
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
                        it->second.childHandles.push_back(element.Handle);
                    }
                } else {
                    m_roots.push_back(element.Handle);
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

        // Pushed outside m_nodesMutex (see PushChangeEvent's comment on
        // lock ordering). Lets a connected lvt.exe eventually react to real
        // events instead of only ever polling via GET_TREE - see
        // IFrameworkConnection::poll_events.
        if (isAdd || isRemove)
            PushChangeEvent(isAdd, element.Handle, relation.Parent, relation.ChildIndex, type, name);
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
