// xaml_diag_common.cpp — Shared XAML diagnostics injection logic
// Used by both XamlProvider and WinUI3Provider.

#include "xaml_diag_common.h"
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
#include <cstdio>
#include <set>
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
    if (el.className == "Microsoft.UI.Content.DesktopChildSiteBridge") {
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

// Single attempt at injection + collection — see inject_and_collect_xaml_tree
// (the public entry point, defined below) for why this is wrapped in a
// retry rather than being the public API directly.
static bool try_inject_and_collect_xaml_tree_once(
    Element& root,
    HWND /*hwnd*/,
    DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel,
    const std::wstring& connPrefix,
    bool fastProperties)
{
    std::wstring tapDll = tap_dll_path(L"lvt_tap");

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lvt: TAP DLL not found: %ls\n", tapDll.c_str());
        return false;
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

    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 1024 * 1024, 10000, &sa));

    if (!pipe) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return false;
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
        return false;
    }

    using FnInit = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto pInit = reinterpret_cast<FnInit>(
        GetProcAddress(hXaml.get(), "InitializeXamlDiagnosticsEx"));
    if (!pInit) {
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx not found in %ls\n", initDllPath.c_str());
        return false;
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
    // "pipe_name|FAST" (see lvt_tap.cpp's SetSiteImpl) — this is the only
    // channel available to tell it which mode to collect in, since
    // InitializeXamlDiagnosticsEx's own parameter list is fixed.
    std::wstring initData = fastProperties ? pipeName + L"|FAST" : pipeName;
    for (int i = 0; i < 10; i++) {
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
        return false;
    }

    if (g_debug)
        fprintf(stderr, "lvt: injection succeeded, waiting for XAML tree data...\n");

    // Wait for the TAP DLL to connect. This is not "waiting for a handshake" —
    // the TAP DLL only calls CreateFileW to connect to this pipe *after* it
    // finishes the entire tree walk (CollectBounds/CollectPositionsAndText/
    // SerializeAndSend — see lvt_tap.cpp's AdviseThreadProcImpl), so this
    // wait covers the whole collection, not just a connection round-trip.
    //
    // kXamlCollectionTimeoutMs used to be 15000 (15s) here, which is what
    // actually caused live, reproducible tree data loss: traced through the
    // TAP DLL's own timestamped log for a real, busy WinUI3 tree (Microsoft
    // Store's animated home page, ~1936 elements) and measured a single
    // *successful* collection (every COM call returned success; the data
    // was built correctly) taking 40.8 seconds end to end — because
    // CollectBounds/CollectPositionsAndText dispatch to the target's UI
    // thread in small chunks specifically so a busy/animating target's own
    // UI thread gets to interleave its own work between chunks (see that
    // dispatch's own comment) rather than being monopolized; an actively
    // animating tree can need many such interleavings, each costing real
    // wall-clock time. At 15 seconds, lvt.exe was routinely giving up and
    // closing this pipe *while the TAP DLL was still legitimately working*,
    // so a bit over half the time it later tried to connect back it found
    // nothing there — "Failed to open pipe: 2" (ERROR_FILE_NOT_FOUND) in the
    // TAP DLL's own log, once its walk finally did finish. From `watch`'s
    // side that showed up as the entire XAML/WinUI3 subtree looking removed
    // for anywhere up to tens of seconds before reappearing once a later
    // tick's collection finished inside the new timeout — reported live as
    // "the tree refreshes/resets" and "the CoreWindow node has no children".
    if (connectErr == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, kXamlCollectionTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            fprintf(stderr, "lvt: TAP DLL did not connect (timeout)\n");
            CancelIo(pipe.get());
            return false;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        fprintf(stderr, "lvt: ConnectNamedPipe failed (error %lu)\n", connectErr);
        return false;
    }

    // Read all data from pipe (overlapped with timeout)
    std::string data;
    char buf[4096];
    DWORD bytesRead = 0;
    OVERLAPPED readOv = {};
    wil::unique_event readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    readOv.hEvent = readEvent.get();
    for (;;) {
        ResetEvent(readOv.hEvent);
        BOOL ok = ReadFile(pipe.get(), buf, sizeof(buf), &bytesRead, &readOv);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(readOv.hEvent, kXamlCollectionTimeoutMs) != WAIT_OBJECT_0) {
                    CancelIo(pipe.get());
                    break;
                }
                if (!GetOverlappedResult(pipe.get(), &readOv, &bytesRead, FALSE) || bytesRead == 0)
                    break;
            } else {
                break;
            }
        } else if (bytesRead == 0) {
            break;
        }
        data.append(buf, bytesRead);
    }

    if (g_debug)
        fprintf(stderr, "lvt: received %zu bytes of XAML tree data\n", data.size());

    if (data.empty()) {
        fprintf(stderr, "lvt: no XAML tree data received from target process\n");
        return false;
    }

    json treeJson;
    try {
        treeJson = json::parse(data);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "lvt: failed to parse XAML tree JSON: %s\n", e.what());
        return false;
    }

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

    return true;
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
        if (try_inject_and_collect_xaml_tree_once(root, hwnd, pid, xamlDiagDll, initDllPath,
                                                  frameworkLabel, connPrefix, fastProperties))
            return true;
    }
    return false;
}

} // namespace lvt
