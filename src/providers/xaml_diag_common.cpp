// xaml_diag_common.cpp — Shared XAML diagnostics injection logic
// Used by both XamlProvider and WinUI3Provider.

#include "xaml_diag_common.h"
#include "../tap/tap_clsid.h"

#include <Windows.h>
#include <xamlOM.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>

using json = nlohmann::json;

namespace lvt {

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

static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
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

// Recursively graft JSON tree nodes into an Element tree.
// parentOffsetX/Y accumulate offsets from the XAML root for screen coordinate computation.
static void graft_json_node(const json& j, Element& parent, const std::string& framework,
                            double parentOffsetX = 0, double parentOffsetY = 0) {
    Element el;
    el.framework = framework;
    el.className = sanitize(j.value("type", ""));
    el.text = sanitize(j.value("name", ""));

    // Simplify type name: "Windows.UI.Xaml.Controls.Button" -> "Button"
    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    // Parse bounds from TAP DLL data
    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double absX = parentOffsetX + ox;
    double absY = parentOffsetY + oy;
    if (w > 0 && h > 0) {
        el.bounds.x = static_cast<int>(absX);
        el.bounds.y = static_cast<int>(absY);
        el.bounds.width = static_cast<int>(w);
        el.bounds.height = static_cast<int>(h);
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_json_node(child, el, framework, absX, absY);
        }
    }

    parent.children.push_back(std::move(el));
}

bool inject_and_collect_xaml_tree(
    Element& root,
    HWND /*hwnd*/,
    DWORD pid,
    const std::wstring& xamlDiagDll,
    const std::wstring& initDllPath,
    const std::string& frameworkLabel)
{
    std::wstring tapDll = get_exe_dir() + L"\\lvt_tap.dll";

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lvt: TAP DLL not found: %ls\n", tapDll.c_str());
        return false;
    }

    std::wstring pipeName = make_pipe_name();
    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 1024 * 1024, 10000, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return false;
    }

    // Load InitializeXamlDiagnosticsEx from the specified DLL.
    // This function runs in OUR process but injects into the target.
    HMODULE hXaml = LoadLibraryExW(initDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hXaml) {
        hXaml = LoadLibraryW(initDllPath.c_str());
    }
    if (!hXaml) {
        fprintf(stderr, "lvt: failed to load %ls (error %lu)\n", initDllPath.c_str(), GetLastError());
        CloseHandle(pipe);
        return false;
    }

    using FnInit = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto pInit = reinterpret_cast<FnInit>(
        GetProcAddress(hXaml, "InitializeXamlDiagnosticsEx"));
    if (!pInit) {
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx not found in %ls\n", initDllPath.c_str());
        FreeLibrary(hXaml);
        CloseHandle(pipe);
        return false;
    }

    // Try both endpoint name prefixes.
    // System XAML: "VisualDiagConnection1", "VisualDiagConnection2", ...
    // WinUI3: "WinUIVisualDiagConnection1", "WinUIVisualDiagConnection2", ...
    static const wchar_t* prefixes[] = {
        L"VisualDiagConnection",
        L"WinUIVisualDiagConnection",
    };
    HRESULT hr = E_FAIL;
    for (auto* prefix : prefixes) {
        for (int i = 0; i < 100; i++) {
            wchar_t endPoint[64];
            swprintf_s(endPoint, L"%s%d", prefix, i + 1);

            hr = pInit(
                endPoint,
                pid,
                xamlDiagDll.c_str(),
                tapDll.c_str(),
                CLSID_LvtTap,
                pipeName.c_str());

            if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                break;
        }
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            break;
    }

    FreeLibrary(hXaml);

    if (FAILED(hr)) {
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx failed (0x%08lX)\n", hr);
        CloseHandle(pipe);
        return false;
    }

    fprintf(stderr, "lvt: injection succeeded, waiting for XAML tree data...\n");

    // Wait for the TAP DLL to connect and send data
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if (!connected) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForSingleObject(ov.hEvent, 15000);
            if (waitResult != WAIT_OBJECT_0) {
                fprintf(stderr, "lvt: TAP DLL did not connect (timeout)\n");
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                CloseHandle(pipe);
                return false;
            }
        } else if (err != ERROR_PIPE_CONNECTED) {
            fprintf(stderr, "lvt: ConnectNamedPipe failed (error %lu)\n", err);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return false;
        }
    }
    CloseHandle(ov.hEvent);

    // Read all data from pipe
    std::string data;
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        data.append(buf, bytesRead);
    }
    CloseHandle(pipe);

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
    // We match them by order since both lists are enumerated in the same order.
    // XAML element offsets are relative to the XAML root; we add the bridge window's
    // screen position to convert to screen coordinates for annotation.
    if (treeJson.is_array()) {
        std::vector<Element*> bridges;
        collect_bridges(root, bridges);

        size_t bridgeIdx = 0;
        for (auto& node : treeJson) {
            std::string typeName = sanitize(node.value("type", ""));
            // Try to graft DesktopWindowXamlSource roots into matching bridges
            if (typeName.find("DesktopWindowXamlSource") != std::string::npos
                && bridgeIdx < bridges.size()) {
                auto* bridge = bridges[bridgeIdx];
                // Use bridge window's screen bounds as coordinate origin for XAML elements
                double baseX = bridge->bounds.x;
                double baseY = bridge->bounds.y;
                graft_json_node(node, *bridge, frameworkLabel, baseX, baseY);
                bridgeIdx++;
            } else {
                // Non-bridge XAML root: graft at the top level
                graft_json_node(node, root, frameworkLabel);
            }
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, frameworkLabel);
    }

    return true;
}

} // namespace lvt
