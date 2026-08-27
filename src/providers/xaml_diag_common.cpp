// xaml_diag_common.cpp — Shared XAML diagnostics injection logic
// Used by both XamlProvider and WinUI3Provider.

#include "xaml_diag_common.h"
#include "framework_connection.h"
#include "../tap/tap_clsid.h"
#include "../debug.h"
#include "../bounds_util.h"

#include "../target.h"
#include "../module_util.h"

#include <Windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <userenv.h>
#include <wil/resource.h>
#include <xamlOM.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#pragma comment(lib, "userenv.lib")

using json = nlohmann::json;

namespace lvt {

// How long to wait for the TAP DLL to finish walking the target's tree and
// connect back with the result — see its use below for the measured,
// evidence-based reason this is 60s, not the 15s it used to be.
static constexpr DWORD kXamlCollectionTimeoutMs = 60000;

static std::wstring make_pipe_name() {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t buf[80];
    swprintf_s(buf, L"\\\\.\\pipe\\lvt_%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

// Check if a process is running inside an AppContainer (UWP).
static bool is_appcontainer_process(DWORD pid) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return false;

    wil::unique_handle token;
    if (!OpenProcessToken(proc.get(), TOKEN_QUERY, token.put()))
        return false;

    BOOL isAppContainer = FALSE;
    DWORD size = sizeof(isAppContainer);
    if (!GetTokenInformation(token.get(), TokenIsAppContainer,
                             &isAppContainer, size, &size))
        return false;
    return isAppContainer != FALSE;
}

// Stage the TAP DLL in a temp directory accessible to AppContainer processes.
// Returns the staged path, or empty on failure.
static std::wstring stage_tap_dll_for_appcontainer(const std::wstring& srcDll) {
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);

    std::wstring destDir = std::wstring(tmpDir) + L"lvt_tap";
    CreateDirectoryW(destDir.c_str(), nullptr);

    // Extract filename from source path
    auto pos = srcDll.find_last_of(L"\\/");
    std::wstring filename = (pos != std::wstring::npos) ? srcDll.substr(pos + 1) : srcDll;
    std::wstring destPath = destDir + L"\\" + filename;

    // Copy, but tolerate ERROR_SHARING_VIOLATION — the DLL is already loaded
    // in the target process from a previous run, which is fine.
    if (!CopyFileW(srcDll.c_str(), destPath.c_str(), FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION &&
            GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (g_debug)
                fprintf(stderr, "lvt: TAP DLL already staged (in use by target)\n");
            return destPath;
        }
        if (g_debug)
            fprintf(stderr, "lvt: failed to stage TAP DLL (error %lu)\n", err);
        return {};
    }

    // Grant ALL_APPLICATION_PACKAGES read+execute access to the directory and DLL
    PSECURITY_DESCRIPTOR rawSd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGX;;;AC)(A;;GRGX;;;WD)", SDDL_REVISION_1, &rawSd, nullptr)) {
        wil::unique_hlocal sd(rawSd);
        PACL dacl = nullptr;
        BOOL daclPresent = FALSE, daclDefaulted = FALSE;
        if (GetSecurityDescriptorDacl(sd.get(), &daclPresent, &dacl, &daclDefaulted) && daclPresent) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(destDir.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr);
            SetNamedSecurityInfoW(const_cast<LPWSTR>(destPath.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr);
        }
    }

    if (g_debug)
        fprintf(stderr, "lvt: staged TAP DLL to %ls\n", destPath.c_str());
    return destPath;
}

// Strip control characters from XAML type names (runtime sometimes includes them)
static std::string sanitize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t')
            r += c;
    }
    return r;
}

// Collect all DesktopChildSiteBridge elements in tree order
static void collect_bridges(Element& el, std::vector<Element*>& bridges) {
    if (el.className == "Microsoft.UI.Content.DesktopChildSiteBridge" ||
        el.className == "Windows.UI.Composition.DesktopWindowContentBridge") {
        bridges.push_back(&el);
    }
    for (auto& child : el.children) {
        collect_bridges(child, bridges);
    }
}

static std::string bridge_identity(const Element& el) {
    if (el.nativeHandle != 0)
        return std::to_string(el.nativeHandle);
    auto it = el.properties.find("hwnd");
    return it == el.properties.end() ? std::string() : it->second;
}

// Recursively graft JSON tree nodes into an Element tree.
// parentOffsetX/Y accumulate offsets from the XAML root for screen coordinate computation.
static void graft_json_node(const json& j, Element& parent, const std::string& framework,
                            double parentOffsetX = 0, double parentOffsetY = 0) {
    parent.children.emplace_back();
    Element& el = parent.children.back();
    el.framework = framework;
    // IXamlDiagnostics supplies an InstanceHandle for every live XAML
    // object. Preserve it as the provider-native identity: watch_diff can
    // reconcile directly by handle, compact element keys can avoid
    // repeating a multi-kilobyte ancestor path, and future property-edit
    // commands can address the exact object expected by
    // IVisualTreeService::SetProperty.
    el.nativeHandle = static_cast<uintptr_t>(j.value("handle", 0ULL));
    el.className = sanitize(j.value("type", ""));

    // x:Name is a developer identifier, not user-visible text — store as property
    std::string xname = sanitize(j.value("name", ""));
    if (!xname.empty()) {
        el.properties["name"] = xname;
    }

    // Simplify type name: "Windows.UI.Xaml.Controls.Button" -> "Button"
    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    // Parse bounds from TAP DLL data.
    // offsetX/offsetY from TransformToVisual are absolute positions within the
    // XAML island, relative to the island root. parentOffsetX/Y is the bridge's
    // screen position, so absX/Y = bridge position + element offset within island.
    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    bool hasOffsetData = j.contains("offsetX") || j.contains("offsetY");
    // TransformToVisual returns absolute position within the island,
    // so use parentOffsetX/Y only as the bridge base (not accumulated).
    double absX = parentOffsetX + ox;
    double absY = parentOffsetY + oy;
    if (w > 0 && h > 0 && hasOffsetData) {
        auto sx = safe_double_to_int(absX);
        auto sy = safe_double_to_int(absY);
        auto sw = safe_double_to_int(w);
        auto sh = safe_double_to_int(h);
        if (sx && sy && sw && sh) {
            el.bounds.x = *sx;
            el.bounds.y = *sy;
            el.bounds.width = *sw;
            el.bounds.height = *sh;
        }
    }

    // Copy additional properties if present in TAP DLL output
    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [key, val] : j["properties"].items()) {
            if (val.is_string()) {
                std::string v = sanitize(val.get<std::string>());
                el.properties[key] = v;
            }
        }
    }
    // Set display text from text-content properties only (not automation names).
    // Remove the source property to avoid duplication in the output (text= vs Text=).
    for (const char* prop : {"Text", "Content", "Header"}) {
        auto it = el.properties.find(prop);
        if (it != el.properties.end() && !it->second.empty()) {
            el.text = it->second;
            el.properties.erase(it);
            break;
        }
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            // Pass bridge base (parentOffsetX/Y) — not accumulated — since
            // TransformToVisual offsets are already absolute within the island
            graft_json_node(child, el, framework, parentOffsetX, parentOffsetY);
        }
    }

}

// Grafts an already-parsed XAML tree JSON payload (as produced by the TAP
// DLL's SerializeAndSend, or a duplex connection's GET_TREE response - same
// shape either way) into `root`. Split out from the connect+collect flow so
// both the one-shot path (inject_and_collect_xaml_tree) and a reused,
// persistent connection's repeated get_tree() calls (see XamlDiagConnection
// below) share exactly one implementation of this logic instead of two
// copies that could drift apart.
static void graft_xaml_tree_json(const json& treeJson, Element& root, const std::string& frameworkLabel) {
    // Graft XAML elements into corresponding bridge windows.
    // Each DesktopWindowXamlSource root maps 1:1 to a DesktopChildSiteBridge HWND.
    // We match by best-fit size: the XAML root's first child dimensions are compared
    // against each bridge's window bounds to find the most compatible match.
    // This is more robust than order-based matching since Win32 HWND enumeration order
    // may differ from XAML tree root enumeration order.
    if (treeJson.is_array()) {
        std::set<std::string> usedBridges;

        // InitializeXamlDiagnosticsEx targets a whole process, not a single
        // HWND: when several top-level windows of the same app share one
        // process (multiple Notepad or File Explorer windows are common —
        // and were exactly how this was found), every window's XAML content
        // comes back in one combined stream, and more than one
        // DesktopWindowXamlSource root here means there is real ambiguity
        // about which root belongs to *this* window's bridges. Only then is
        // it worth rejecting a low-confidence match (see the tolerance
        // check below): with a single root there is nothing else it could
        // be, no matter how poorly its content size happens to match, so
        // the legacy graft-under-root fallback stays exactly as
        // conservative as before for the overwhelmingly common case.
        size_t xamlSourceRootCount = 0;
        for (auto& node : treeJson) {
            if (sanitize(node.value("type", "")).find("DesktopWindowXamlSource") != std::string::npos)
                xamlSourceRootCount++;
        }
        const bool multipleRootsAmbiguous = xamlSourceRootCount > 1;

        for (auto& node : treeJson) {
            std::string typeName = sanitize(node.value("type", ""));
            if (typeName.find("DesktopWindowXamlSource") == std::string::npos) {
                graft_json_node(node, root, frameworkLabel, root.bounds.x, root.bounds.y);
                continue;
            }

            // Get the XAML content dimensions from descendants with bounds
            double contentW = 0, contentH = 0;
            std::function<void(const json&)> findContentSize = [&](const json& n) {
                double w = n.value("width", 0.0);
                double h = n.value("height", 0.0);
                if (w > contentW) contentW = w;
                if (h > contentH) contentH = h;
                if (n.contains("children") && n["children"].is_array()) {
                    for (auto& child : n["children"]) {
                        findContentSize(child);
                        if (contentW > 0 && contentH > 0) return; // found, stop early
                    }
                }
            };
            if (node.contains("children") && node["children"].is_array()) {
                for (auto& child : node["children"]) {
                    findContentSize(child);
                }
            }

            // Skip strict bridge matching for roots with no measurable
            // content when there is no ambiguity to resolve: fall back to
            // the legacy graft-under-root behavior, same as before this
            // window's-worth-of-contamination fix existed, since with a
            // single root there is no sibling window's content it could be
            // confused with — dropping it here would only lose real
            // structure (e.g. a not-yet-laid-out tab strip) for no safety
            // benefit. Only drop outright when multiple roots are actually
            // competing for the same bridges.
            if (contentW <= 0 && contentH <= 0) {
                if (!multipleRootsAmbiguous)
                    graft_json_node(node, root, frameworkLabel, root.bounds.x, root.bounds.y);
                continue;
            }

            if (g_debug) {
                fprintf(stderr, "lvt: XAML root contentW=%.0f contentH=%.0f\n",
                        contentW, contentH);
            }

            // Find the best-matching bridge by size similarity
            std::vector<Element*> bridges;
            collect_bridges(root, bridges);
            int bestIdx = -1;
            double bestScore = 1e18;
            for (size_t i = 0; i < bridges.size(); i++) {
                auto identity = bridge_identity(*bridges[i]);
                if (!identity.empty() && usedBridges.count(identity)) continue;
                double bw = bridges[i]->bounds.width;
                double bh = bridges[i]->bounds.height;
                // Score: prefer bridges whose dimensions best accommodate the content
                double wDiff = std::abs(bw - contentW);
                double hDiff = std::abs(bh - contentH);
                double score = wDiff + hDiff;
                if (score < bestScore) {
                    bestScore = score;
                    bestIdx = static_cast<int>(i);
                }
            }

            // Reject a "best" match that still isn't actually close — but
            // only when multiple roots are genuinely competing (see
            // multipleRootsAmbiguous above). Without a tolerance, the loop
            // above always finds *some* bridge — including bridges
            // belonging to *this* window that just happen to be the
            // least-bad leftover for a completely different window's root —
            // silently grafting one window's content (and its Text/bounds)
            // onto a sibling window's tree. Requiring the winning candidate
            // to be within a size-relative tolerance of its own bridge is
            // what tells "this genuinely is that bridge's content" apart
            // from "this is a foreign root that merely didn't lose by
            // much"; anything else is dropped instead of misattached. When
            // there is only one root, skip this check entirely and keep the
            // legacy behavior of accepting whatever the single bridge is,
            // however poor the size match — there is no other candidate it
            // could rightfully belong to.
            if (multipleRootsAmbiguous && bestIdx >= 0) {
                double bw = bridges[bestIdx]->bounds.width;
                double bh = bridges[bestIdx]->bounds.height;
                constexpr double kMinAbsoluteToleragePx = 40.0;
                constexpr double kRelativeTolerance = 0.25;
                double tolerance = std::max(kMinAbsoluteToleragePx, (bw + bh) * kRelativeTolerance);
                if (bestScore > tolerance) {
                    if (g_debug) {
                        fprintf(stderr, "lvt: rejecting XAML root match (score=%.0f > tolerance=%.0f); "
                                        "likely belongs to a different window sharing this process\n",
                                bestScore, tolerance);
                    }
                    bestIdx = -1;
                }
            }

            if (bestIdx >= 0) {
                auto* bridge = bridges[bestIdx];
                auto identity = bridge_identity(*bridge);
                if (!identity.empty())
                    usedBridges.insert(identity);
                double baseX = bridge->bounds.x;
                double baseY = bridge->bounds.y;
                graft_json_node(node, *bridge, frameworkLabel, baseX, baseY);
            } else if (!multipleRootsAmbiguous) {
                // No DesktopChildSiteBridge matched at all — including the
                // case where this window has none to begin with (classic
                // system XAML doesn't use the WinUI3 Islands bridge model,
                // so `bridges` is always empty there). With only one root in
                // play there is no sibling window's content to confuse this
                // with, so fall back to the legacy graft-under-root
                // behavior exactly as before this fix.
                graft_json_node(node, root, frameworkLabel, root.bounds.x, root.bounds.y);
            }
            // Otherwise: multiple roots were genuinely competing and none
            // matched confidently enough — drop this root rather than
            // misattach it to the wrong window.
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, frameworkLabel);
    }
}

// Buffered line I/O over the persistent duplex pipe lvt.exe creates and the
// TAP DLL connects back to. Every read/write must pass an OVERLAPPED
// structure - the pipe handle is created with FILE_FLAG_OVERLAPPED (needed
// so the initial "wait for the TAP DLL to connect" step can be bounded by a
// timeout) and mixing overlapped and non-overlapped calls on the same
// handle is unsupported.
class DuplexPipeLineIO {
public:
    explicit DuplexPipeLineIO(HANDLE pipe) : m_pipe(pipe) {
        m_readEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        m_writeEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    }

    // Reads one '\n'-terminated line (returned without the newline).
    // Returns false on timeout, a broken pipe, or EOF - all of which mean
    // this connection is no longer usable.
    bool read_line(DWORD timeoutMs, std::string& outLine) {
        for (;;) {
            auto nl = m_buffer.find('\n');
            if (nl != std::string::npos) {
                outLine = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
                return true;
            }
            char chunk[8192];
            DWORD bytesRead = 0;
            OVERLAPPED ov = {};
            ResetEvent(m_readEvent.get());
            ov.hEvent = m_readEvent.get();
            BOOL ok = ReadFile(m_pipe, chunk, sizeof(chunk), &bytesRead, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(m_readEvent.get(), timeoutMs) != WAIT_OBJECT_0) {
                        CancelIo(m_pipe);
                        return false;
                    }
                    if (!GetOverlappedResult(m_pipe, &ov, &bytesRead, FALSE) || bytesRead == 0)
                        return false;
                } else {
                    return false;
                }
            } else if (bytesRead == 0) {
                return false;
            }
            m_buffer.append(chunk, bytesRead);
        }
    }

    // Writes one line (message + '\n'). lvt.exe only ever sends short text
    // commands, so a generous fixed default timeout is plenty.
    bool write_line(const std::string& line, DWORD timeoutMs = 5000) {
        std::string withNewline = line + "\n";
        OVERLAPPED ov = {};
        ResetEvent(m_writeEvent.get());
        ov.hEvent = m_writeEvent.get();
        DWORD written = 0;
        BOOL ok = WriteFile(m_pipe, withNewline.data(),
                             static_cast<DWORD>(withNewline.size()), &written, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(m_writeEvent.get(), timeoutMs) != WAIT_OBJECT_0) {
                CancelIo(m_pipe);
                return false;
            }
            if (!GetOverlappedResult(m_pipe, &ov, &written, FALSE)) return false;
        }
        return true;
    }

private:
    HANDLE m_pipe;
    wil::unique_event m_readEvent;
    wil::unique_event m_writeEvent;
    std::string m_buffer;
};

// A live, persistent connection to one XAML/WinUI3 diagnostics session in a
// target process - the concrete IFrameworkConnection this file provides.
// See framework_connection.h for why this exists: InitializeXamlDiagnosticsEx
// and AdviseVisualTreeChange are meant to be called ONCE per session, not
// re-run from scratch on every tree refresh (the old design, and the
// confirmed source of an unbounded per-tick resource leak in the TAP DLL -
// one message-only window created and never destroyed per refresh).
//
// connect() performs the injection exactly once; get_tree() then reuses the
// same pipe for as many refreshes as the caller needs, and the destructor
// sends a clean DISCONNECT so the TAP DLL's own teardown
// (UnadviseVisualTreeChange, DestroyWindow, COM release - see lvt_tap.cpp's
// CleanupUIResources) runs exactly once, when this connection actually ends,
// instead of never running at all.
class XamlDiagConnection : public IFrameworkConnection {
public:
    static std::shared_ptr<XamlDiagConnection> connect(
        HWND hwnd, DWORD pid,
        const std::wstring& xamlDiagDll,
        const std::wstring& initDllPath,
        std::string frameworkLabel,
        const std::wstring& connPrefix);

    ~XamlDiagConnection() override {
        if (m_alive && m_io) {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            // Best-effort: tell the TAP DLL we're done so it runs its clean
            // teardown instead of just noticing a broken pipe later. A
            // short timeout is fine - we are tearing down either way.
            m_io->write_line("DISCONNECT", 2000);
        }
    }

    bool get_tree(Element& root, bool fastProperties,
                  const std::string& /*providerOption*/ = {}) override {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (!m_alive) return false;
        std::string cmd = fastProperties ? "GET_TREE FAST" : "GET_TREE";
        if (!m_io->write_line(cmd)) {
            m_alive = false;
            return false;
        }
        // A pushed CHANGE event (see lvt_tap.cpp's OnVisualTreeChange/
        // PushChangeEvent) can arrive on this same stream at any time,
        // interleaved with the response to this specific request - drain
        // and queue any of those (they start with '{') before the actual
        // tree response (a JSON array, starts with '[') turns up.
        for (;;) {
            std::string line;
            if (!m_io->read_line(kXamlCollectionTimeoutMs, line)) {
                fprintf(stderr, "lvt: %s: no response from TAP DLL (timeout or broken connection)\n",
                        m_frameworkLabel.c_str());
                m_alive = false;
                return false;
            }
            if (!line.empty() && line[0] == '{') {
                queue_change_event(line);
                continue;
            }
            json treeJson;
            try {
                treeJson = json::parse(line);
            } catch (const json::parse_error& e) {
                fprintf(stderr, "lvt: failed to parse XAML tree JSON: %s\n", e.what());
                return false;
            }
            graft_xaml_tree_json(treeJson, root, m_frameworkLabel);
            return true;
        }
    }

    std::vector<ConnectionEvent> poll_events() override {
        std::lock_guard<std::mutex> lock(m_eventsMutex);
        return std::move(m_pendingEvents);
    }

    bool is_alive() const override { return m_alive; }

    FrameworkPropertyResult get_properties(uintptr_t handle) override {
        const auto commandId = next_command_id();
        return send_property_command(
            "GET_PROPERTIES " + std::to_string(commandId) + " " +
            std::to_string(handle), commandId);
    }

    FrameworkPropertyResult set_property(
        uintptr_t handle, uint32_t propertyIndex,
        const std::string& valueType, const std::string& value) override {
        const auto commandId = next_command_id();
        std::ostringstream command;
        command << "SET_PROPERTY " << commandId << " " << handle << " "
                << propertyIndex << " " << hex_encode(valueType) << " "
                << hex_encode(value);
        return send_property_command(command.str(), commandId);
    }

    FrameworkPropertyResult clear_property(
        uintptr_t handle, uint32_t propertyIndex) override {
        const auto commandId = next_command_id();
        std::ostringstream command;
        command << "CLEAR_PROPERTY " << commandId << " " << handle << " "
                << propertyIndex;
        return send_property_command(command.str(), commandId);
    }

private:
    XamlDiagConnection(wil::unique_hfile pipe, std::unique_ptr<DuplexPipeLineIO> io,
                       std::string frameworkLabel)
        : m_pipe(std::move(pipe)), m_io(std::move(io)), m_frameworkLabel(std::move(frameworkLabel)) {
        m_alive = true;
    }

    // Parses one {"type":"CHANGE",...} line (see lvt_tap.cpp's
    // PushChangeEvent for the exact shape) and queues it for poll_events().
    // Malformed/unrecognized lines are dropped rather than treated as an
    // error - a push event is best-effort by design (see PushChangeEvent's
    // comment), and get_tree()'s own response is never affected by this.
    void queue_change_event(const std::string& line) {
        json ev;
        try {
            ev = json::parse(line);
        } catch (const json::parse_error&) {
            return;
        }
        if (ev.value("type", "") != "CHANGE")
            return;

        ConnectionEvent ce;
        ce.mutation = (ev.value("mutation", "") == "remove")
                          ? ConnectionEvent::Mutation::removed
                          : ConnectionEvent::Mutation::added;
        ce.handle = static_cast<uintptr_t>(ev.value("handle", 0ULL));
        ce.parentHandle = static_cast<uintptr_t>(ev.value("parent", 0ULL));
        ce.childIndex = ev.value("childIndex", 0);
        ce.elementType = ev.value("elementType", "");
        ce.name = ev.value("name", "");

        std::lock_guard<std::mutex> lock(m_eventsMutex);
        // A caller that never calls poll_events() at all (e.g. a one-shot
        // CLI command that happened to acquire a connection but never asked
        // for events) must not turn this into an unbounded leak of its own.
        // Capping and dropping the oldest is safe: nothing currently
        // depends on poll_events() for correctness (get_tree() is always a
        // complete, independent refresh), only as an optional efficiency
        // gain for a caller that does drain regularly.
        constexpr size_t kMaxPendingEvents = 10000;
        if (m_pendingEvents.size() >= kMaxPendingEvents)
            m_pendingEvents.erase(m_pendingEvents.begin());
        m_pendingEvents.push_back(std::move(ce));
    }

    static std::string hex_encode(const std::string& value) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (unsigned char ch : value) {
            encoded.push_back(digits[ch >> 4]);
            encoded.push_back(digits[ch & 0x0F]);
        }
        // A dash represents an empty value. An empty token would disappear
        // when the TAP's command parser splits on whitespace.
        return encoded.empty() ? "-" : encoded;
    }

    uint64_t next_command_id() {
        return m_nextCommandId.fetch_add(1);
    }

    static HRESULT parse_hresult(const json& response) {
        auto it = response.find("hresult");
        if (it == response.end() || !it->is_string())
            return response.value("ok", false) ? S_OK : E_FAIL;
        const std::string text = it->get<std::string>();
        const char* first = text.data();
        if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
            first += 2;
        uint32_t raw = 0;
        auto parsed = std::from_chars(first, text.data() + text.size(), raw, 16);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()
                   ? static_cast<HRESULT>(raw)
                   : E_FAIL;
    }

    FrameworkPropertyResult send_property_command(
        const std::string& command, uint64_t expectedCommandId) {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        FrameworkPropertyResult result;
        if (!m_alive) {
            result.hresult = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
            result.error = "The XAML diagnostics connection is no longer available";
            return result;
        }

        if (expectedCommandId == 0 || !m_io->write_line(command)) {
            m_alive = false;
            result.hresult = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
            result.error = "Could not send the property command to the TAP DLL";
            return result;
        }

        for (;;) {
            std::string line;
            if (!m_io->read_line(kXamlCollectionTimeoutMs, line)) {
                m_alive = false;
                result.hresult = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                result.error = "Timed out waiting for the TAP DLL property response";
                return result;
            }

            json response = json::parse(line, nullptr, false);
            if (response.is_discarded() || !response.is_object())
                continue;
            if (response.value("type", "") == "CHANGE") {
                queue_change_event(line);
                continue;
            }
            if (response.value("type", "") != "PROPERTY_RESULT" ||
                response.value("commandId", uint64_t{0}) != expectedCommandId) {
                continue;
            }

            result.ok = response.value("ok", false);
            result.hresult = parse_hresult(response);
            result.error = response.value("error", "");
            if (auto value = response.find("value");
                value != response.end() && value->is_string()) {
                result.hasValue = true;
                result.value = value->get<std::string>();
            }
            if (auto properties = response.find("properties");
                properties != response.end() && properties->is_array()) {
                result.hasProperties = true;
                for (const auto& item : *properties) {
                    if (!item.is_object())
                        continue;
                    FrameworkProperty property;
                    property.name = item.value("name", "");
                    property.value = item.value("value", "");
                    property.valueType = item.value("valueType", "");
                    property.declaringType = item.value("declaringType", "");
                    property.propertyIndex = item.value("propertyIndex", uint32_t{0});
                    property.metadataBits = item.value("metadataBits", uint64_t{0});
                    property.overridden = item.value("overridden", false);
                    property.source = item.value("source", "");
                    result.properties.push_back(std::move(property));
                }
            }
            return result;
        }
    }

    wil::unique_hfile m_pipe;
    std::unique_ptr<DuplexPipeLineIO> m_io;
    std::string m_frameworkLabel;
    std::atomic_bool m_alive = false;
    std::atomic_uint64_t m_nextCommandId = 1;
    std::mutex m_commandMutex;
    std::mutex m_eventsMutex;
    std::vector<ConnectionEvent> m_pendingEvents;
};

std::shared_ptr<XamlDiagConnection> XamlDiagConnection::connect(
    HWND /*hwnd*/, DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    std::string frameworkLabel,
    const std::wstring& connPrefix)
{
    std::wstring tapDll = tap_dll_path(L"lvt_tap");

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lvt: TAP DLL not found: %ls\n", tapDll.c_str());
        return nullptr;
    }

    // AppContainer (UWP) processes can't load DLLs from arbitrary paths.
    // Stage the TAP DLL in a temp directory with appropriate ACLs.
    std::wstring stagedDll;
    if (is_appcontainer_process(pid)) {
        stagedDll = stage_tap_dll_for_appcontainer(tapDll);
        if (!stagedDll.empty())
            tapDll = stagedDll;
    }

    std::wstring pipeName = make_pipe_name();

    // Build a security descriptor that allows AppContainer (UWP) processes to connect.
    // S-1-15-2-1 = ALL_APPLICATION_PACKAGES
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR rawPipeSd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1, &rawPipeSd, nullptr);
    wil::unique_hlocal pipeSecurityDescriptor(rawPipeSd);
    sa.lpSecurityDescriptor = pipeSecurityDescriptor.get();

    // PIPE_ACCESS_DUPLEX (not PIPE_ACCESS_INBOUND): lvt.exe now sends
    // GET_TREE/DISCONNECT requests over this same pipe for as long as the
    // connection lives, not just receiving one write-once blob.
    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 1024 * 1024, 1024 * 1024, 10000, &sa));

    if (!pipe) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return nullptr;
    }

    // Load InitializeXamlDiagnosticsEx from the specified DLL.
    // This function runs in OUR process but injects into the target.
    wil::unique_hmodule hXaml(LoadLibraryExW(initDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if (!hXaml) {
        hXaml.reset(LoadLibraryW(initDllPath.c_str()));
    }
    if (!hXaml) {
        fprintf(stderr, "lvt: failed to load %ls (error %lu)\n", initDllPath.c_str(), GetLastError());
        return nullptr;
    }

    using FnInit = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto pInit = reinterpret_cast<FnInit>(
        GetProcAddress(hXaml.get(), "InitializeXamlDiagnosticsEx"));
    if (!pInit) {
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx not found in %ls\n", initDllPath.c_str());
        return nullptr;
    }

    // Try connection endpoint names: prefix + "1", prefix + "2", ...
    // Start the overlapped pipe connect BEFORE injection. When the TAP DLL
    // is already loaded in the target (repeated runs), pInit triggers SetSite
    // synchronously and the TAP DLL can connect+write+close before we'd
    // otherwise reach ConnectNamedPipe, causing a missed connection.
    OVERLAPPED ov = {};
    wil::unique_event connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ov.hEvent = connectEvent.get();
    ConnectNamedPipe(pipe.get(), &ov);
    DWORD connectErr = GetLastError();

    HRESULT hr = E_FAIL;
    // The TAP DLL parses its GetInitializationData() BSTR as "pipe_name" or
    // "pipe_name|FAST" (see lvt_tap.cpp's SetSiteImpl) — this only sets the
    // connection-wide *default* fast mode now; get_tree() overrides it per
    // request (see HandleGetTree in lvt_tap.cpp), so a single persistent
    // connection can still mix fast live-tree polls with an occasional full
    // request the way the old per-call model did.
    std::wstring initData = pipeName;
    // Connection identifiers are monotonically allocated by each XAML core
    // and can grow well beyond 10 in a long-lived, multi-window process.
    // Windows Terminal was observed with no endpoint in slots 1..10 despite
    // an active WinUI tree. UWPSpy uses the same 10,000-attempt ceiling,
    // citing DXamlCore's own allocation behavior; keep a high finite bound so
    // a framework-detection false positive cannot loop forever.
    constexpr int kMaxConnectionIdentifiers = 10000;
    for (int i = 0; i < kMaxConnectionIdentifiers; i++) {
        wchar_t endPoint[64];
        swprintf_s(endPoint, L"%s%d", connPrefix.c_str(), i + 1);

        hr = pInit(
            endPoint,
            pid,
            xamlDiagDll.c_str(),
            tapDll.c_str(),
            CLSID_LvtTap,
            initData.c_str());

        if (g_debug)
            fprintf(stderr, "lvt: %ls pid=%lu -> 0x%08lX\n", endPoint, pid, hr);

        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            break;
    }

    hXaml.reset();

    if (FAILED(hr)) {
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx failed (0x%08lX)\n", hr);
        CancelIo(pipe.get());
        return nullptr;
    }

    if (g_debug)
        fprintf(stderr, "lvt: injection succeeded, waiting for TAP DLL to connect...\n");

    // Wait for the TAP DLL to connect and subscribe. Unlike the old
    // one-shot model, this is now a real handshake wait, not a "wait for
    // the whole collection" wait: the TAP DLL sends READY as soon as it has
    // subscribed (AdviseVisualTreeChange), *before* doing any bounds/
    // property collection (see lvt_tap.cpp's ServeConnection) - the actual
    // per-request collection cost (measured live at up to 40+ seconds for a
    // large, actively animating tree - see kXamlCollectionTimeoutMs's own
    // comment for that measurement) is now bounded by get_tree()'s own
    // read_line() timeout instead of this connect step.
    if (connectErr == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, kXamlCollectionTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            fprintf(stderr, "lvt: TAP DLL did not connect (timeout)\n");
            CancelIo(pipe.get());
            return nullptr;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        fprintf(stderr, "lvt: ConnectNamedPipe failed (error %lu)\n", connectErr);
        return nullptr;
    }

    auto io = std::make_unique<DuplexPipeLineIO>(pipe.get());
    std::string readyLine;
    if (!io->read_line(kXamlCollectionTimeoutMs, readyLine) || readyLine != "READY") {
        fprintf(stderr, "lvt: TAP DLL did not send READY (got '%s')\n", readyLine.c_str());
        return nullptr;
    }

    if (g_debug)
        fprintf(stderr, "lvt: TAP DLL connected and ready\n");

    // std::shared_ptr with a private constructor: std::make_shared can't
    // call it directly, so construct with new and wrap.
    return std::shared_ptr<XamlDiagConnection>(
        new XamlDiagConnection(std::move(pipe), std::move(io), std::move(frameworkLabel)));
}

// Establishes a persistent XAML/WinUI3 diagnostics connection for reuse
// across many tree refreshes - see framework_connection.h and
// connection_registry.h for how a caller (watch's loop, an MCP session)
// acquires/reuses/releases one instead of re-injecting per refresh.
std::shared_ptr<IFrameworkConnection> make_xaml_diag_connection(
    HWND hwnd, DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel,
    const std::wstring& connPrefix)
{
    return XamlDiagConnection::connect(hwnd, pid, xamlDiagDll, initDllPath, frameworkLabel, connPrefix);
}

// Injects and collects the XAML tree, retrying a small, bounded number of
// times if the single attempt genuinely fails (a hard error, not merely
// "this is taking a while" — kXamlCollectionTimeoutMs above already gives a
// slow-but-progressing collection the room it needs, based on measured
// real-world timing, so a failure that gets here is a rarer case: the
// target closing mid-walk, a one-off COM error, and similar).
//
// Deliberately few attempts, with a real gap between them, rather than
// many with a short one: if a previous attempt failed after actually
// starting a walk inside the target (as opposed to failing before that,
// e.g. at InitializeXamlDiagnosticsEx itself), the TAP DLL's own worker
// thread for that attempt keeps running in the target to completion
// regardless of what lvt.exe decides to do — there is no way to cancel it
// from here. Retrying too eagerly would start a *second*, fully
// independent walk competing with that still-running one for the same
// target UI thread via SendMessage, which can only make a target that is
// already struggling to keep up slower still, not faster — the opposite
// of what a retry is supposed to achieve. A short pause before the one
// retry this makes gives a stray straggler more of a chance to finish
// first.
bool inject_and_collect_xaml_tree(
    Element& root,
    HWND hwnd,
    DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel,
    const std::wstring& connPrefix,
    bool fastProperties)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            if (g_debug)
                fprintf(stderr, "lvt: retrying XAML injection (attempt %d)\n", attempt + 1);
            Sleep(1000);
        }
        // A one-shot caller (dump/query/screenshot) has no reason to keep
        // this connection open once it has its tree - unlike watch's loop
        // or an MCP session (see connection_registry.h), which acquire one
        // and reuse it across many refreshes instead of reconnecting every
        // time. `connection` going out of scope at the end of this
        // iteration sends a clean DISCONNECT (see ~XamlDiagConnection).
        auto connection = XamlDiagConnection::connect(hwnd, pid, xamlDiagDll, initDllPath,
                                                       frameworkLabel, connPrefix);
        if (connection && connection->get_tree(root, fastProperties))
            return true;
    }
    return false;
}

} // namespace lvt
