// xaml_diag_common.cpp — Shared XAML diagnostics injection logic
// Used by both XamlProvider and WinUI3Provider.

#include "xaml_diag_common.h"
#include "../tap/tap_clsid.h"

#include <Windows.h>
#include <xamlOM.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <random>

using json = nlohmann::json;

namespace lvt {

// Generate a unique pipe name for this diagnostics session
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

// Get the directory of the running executable
static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
}

// Recursively graft JSON tree nodes into an Element tree
static void graft_json_node(const json& j, Element& parent, const std::string& framework) {
    Element el;
    el.framework = framework;
    el.className = j.value("type", "");
    el.text = j.value("name", "");

    // Simplify type name: "Windows.UI.Xaml.Controls.Button" -> "Button"
    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [k, v] : j["properties"].items()) {
            el.properties[k] = v.get<std::string>();
        }
        // Use Name/x:Name as text if element text is empty
        if (el.text.empty()) {
            if (el.properties.count("Name")) el.text = el.properties["Name"];
            else if (el.properties.count("x:Name")) el.text = el.properties["x:Name"];
        }
        // Use Text/Content property as text if still empty
        if (el.text.empty()) {
            if (el.properties.count("Text")) el.text = el.properties["Text"];
            else if (el.properties.count("Content")) el.text = el.properties["Content"];
        }
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_json_node(child, el, framework);
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
    // Resolve TAP DLL path (next to lvt.exe)
    std::wstring tapDll = get_exe_dir() + L"\\lvt_tap.dll";

    // Verify TAP DLL exists
    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "lvt: TAP DLL not found: %ls\n", tapDll.c_str());
        return false;
    }

    // Create named pipe server
    std::wstring pipeName = make_pipe_name();
    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // max instances
        0,          // out buffer
        1024 * 1024, // in buffer (1 MB)
        5000,       // timeout ms
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return false;
    }

    // Load InitializeXamlDiagnosticsEx from the specified DLL
    HMODULE hXaml = LoadLibraryExW(initDllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hXaml) {
        // Try without search flags (for full paths)
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
        fprintf(stderr, "lvt: InitializeXamlDiagnosticsEx not found\n");
        FreeLibrary(hXaml);
        CloseHandle(pipe);
        return false;
    }

    // Call InitializeXamlDiagnosticsEx — this injects lvt_tap.dll into the target.
    // System XAML registers connections as "VisualDiagConnection1", "VisualDiagConnection2", ...
    // WinUI3 registers as "WinUIVisualDiagConnection1", "WinUIVisualDiagConnection2", ...
    // We try both prefixes until we find the active connection.
    // Reference: microsoft/microsoft-ui-xaml DXamlCore.cpp RegisterVisualDiagnosticsPort()
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
                pipeName.c_str());  // init data = pipe name

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

    // Wait for the TAP DLL to connect and send data
    // Use overlapped I/O with a timeout
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(pipe, &ov);
    DWORD waitResult = WaitForSingleObject(ov.hEvent, 10000); // 10 second timeout
    CloseHandle(ov.hEvent);

    if (waitResult != WAIT_OBJECT_0) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            fprintf(stderr, "lvt: TAP DLL did not connect (timeout or error %lu)\n", err);
            CloseHandle(pipe);
            return false;
        }
    }

    // Read all data from pipe
    std::string data;
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        data.append(buf, bytesRead);
    }
    CloseHandle(pipe);

    if (data.empty()) {
        fprintf(stderr, "lvt: no XAML tree data received from target process\n");
        return false;
    }

    // Parse JSON
    json treeJson;
    try {
        treeJson = json::parse(data);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "lvt: failed to parse XAML tree JSON: %s\n", e.what());
        return false;
    }

    // Graft XAML elements into the root element tree
    if (treeJson.is_array()) {
        for (auto& node : treeJson) {
            graft_json_node(node, root, frameworkLabel);
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, frameworkLabel);
    }

    return true;
}

} // namespace lvt
