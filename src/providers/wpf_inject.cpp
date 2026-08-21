// wpf_inject.cpp — WPF DLL injection and tree collection.
// Injects lvt_wpf_tap_x64.dll into the target process via
// CreateRemoteThread + LoadLibraryW, then reads the WPF visual tree
// JSON over a named pipe.

#include "wpf_inject.h"
#include "../debug.h"
#include "../target.h"
#include "../bounds_util.h"
#include "../module_util.h"

#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <aclapi.h>
#include <Psapi.h>
#include <wil/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cmath>
#include <string>
#include <fstream>

using json = nlohmann::json;

namespace lvt {

static std::wstring make_pipe_name() {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t buf[80];
    swprintf_s(buf, L"\\\\.\\pipe\\lvt_wpf_%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}


static std::string sanitize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t')
            r += c;
    }
    return r;
}

// Recursively graft JSON tree nodes into an Element tree.
static void graft_json_node(const json& j, Element& parent, const std::string& framework) {
    Element el;
    el.framework = framework;
    el.className = sanitize(j.value("type", ""));

    // Simplify type name: "System.Windows.Controls.Button" -> "Button"
    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    auto name = sanitize(j.value("name", ""));
    if (!name.empty())
        el.properties["name"] = name;

    // A "text" key present but empty (WpfTreeWalker sends this for a real
    // string-typed Text/Content/Header property that just happens to be
    // empty) is real data and must not be overwritten by the name fallback;
    // only an absent key - no such property at all - falls back to name.
    if (j.contains("text") && j["text"].is_string())
        el.text = sanitize(j["text"].get<std::string>());
    else
        el.text = name;

    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    if (w > 0 && h > 0) {
        auto sx = safe_double_to_int(ox);
        auto sy = safe_double_to_int(oy);
        auto sw = safe_double_to_int(w);
        auto sh = safe_double_to_int(h);
        if (sx && sy && sw && sh) {
            el.bounds.x = *sx;
            el.bounds.y = *sy;
            el.bounds.width = *sw;
            el.bounds.height = *sh;
        }
    } else if (j.contains("zeroSize") && j["zeroSize"].is_boolean() && j["zeroSize"].get<bool>()) {
        // WpfTreeWalker read ActualWidth/ActualHeight and they really were
        // zero (or negative, pre-layout) - distinct from bounds being absent
        // for some other reason (e.g. its PresentationSource lookup threw).
        el.properties["zeroSize"] = "true";
    }

    // Visibility/enabled as properties.
    //
    // "visible" stays exactly as before: it is the generic, provider-agnostic
    // signal lvt_api.cpp's click-safety check reads across Win32/WinForms/WPF
    // alike, so its boolean shape cannot change here in isolation.
    //
    // "wpf.visibility" is additive: WPF has three visibilities, and this is
    // the only place that carries Hidden vs Collapsed through to the Element
    // tree, matching the "winforms.visible" precedent for framework-specific
    // detail that the generic key does not carry.
    if (j.contains("visible") && j["visible"].is_boolean() && !j["visible"].get<bool>())
        el.properties["visible"] = "false";
    if (j.contains("wpf.visibility") && j["wpf.visibility"].is_string())
        el.properties["wpf.visibility"] = j["wpf.visibility"].get<std::string>();
    if (j.contains("enabled") && j["enabled"].is_boolean() && !j["enabled"].get<bool>())
        el.properties["enabled"] = "false";

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_json_node(child, el, framework);
        }
    }

    parent.children.push_back(std::move(el));
}

std::vector<Element> wpf_parse_tree_json(const std::string& jsonText, const std::string& framework) {
    json treeJson;
    try {
        treeJson = json::parse(jsonText);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "lvt: failed to parse WPF tree JSON: %s\n", e.what());
        return {};
    }

    // graft_json_node appends to a parent's children, so a synthetic parent
    // collects the top-level roots (the JSON is an array of Window roots, or
    // occasionally a single object) without needing a second code path here.
    Element syntheticParent;
    if (treeJson.is_array()) {
        for (auto& node : treeJson) {
            graft_json_node(node, syntheticParent, framework);
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, syntheticParent, framework);
    }
    return std::move(syntheticParent.children);
}

// Write pipe name to a sidecar file next to the TAP DLL so it can read it
static bool write_pipe_name_file(const std::wstring& dir, const std::wstring& pipeName) {
    std::wstring path = dir + L"\\lvt_wpf_pipe.txt";
    // Convert pipe name to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, utf8.data(), len, nullptr, nullptr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8.data(), utf8.size());
    return true;
}

// Inject a DLL into a remote process via CreateRemoteThread + LoadLibraryW
static bool inject_dll(DWORD pid, const std::wstring& dllPath) {
    wil::unique_handle proc(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, pid));
    if (!proc) {
        fprintf(stderr, "lvt: failed to open target process %lu (error %lu)\n", pid, GetLastError());
        return false;
    }

    size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(proc.get(), nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) {
        fprintf(stderr, "lvt: VirtualAllocEx failed (error %lu)\n", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(proc.get(), remoteMem, dllPath.c_str(), pathBytes, nullptr)) {
        fprintf(stderr, "lvt: WriteProcessMemory failed (error %lu)\n", GetLastError());
        VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    wil::unique_handle thread(CreateRemoteThread(
        proc.get(), nullptr, 0, loadLibAddr, remoteMem, 0, nullptr));
    if (!thread) {
        fprintf(stderr, "lvt: CreateRemoteThread failed (error %lu)\n", GetLastError());
        VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);
        return false;
    }

    // Wait for the DLL to load (5 second timeout)
    WaitForSingleObject(thread.get(), 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread.get(), &exitCode);
    VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);

    // exitCode is the HMODULE returned by LoadLibraryW (0 = failed)
    if (exitCode == 0) {
        fprintf(stderr, "lvt: LoadLibraryW failed in target process\n");
        return false;
    }

    if (g_debug)
        fprintf(stderr, "lvt: WPF TAP DLL injected into pid %lu\n", pid);
    return true;
}

bool inject_and_collect_wpf_tree(Element& root, HWND /*hwnd*/, DWORD pid) {
    // Check target process bitness matches ours
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (proc) {
        BOOL isWow64 = FALSE;
        if (IsWow64Process(proc.get(), &isWow64) && isWow64) {
#if defined(_M_X64) || defined(_M_ARM64)
            fprintf(stderr,
                "lvt: WPF target is 32-bit (WoW64) - run lvt-x86.exe instead\n");
            return false;
#endif
        }
    }

    std::wstring exeDir = get_tap_directory();
    std::wstring tapDll = tap_dll_path(L"lvt_wpf_tap");

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (g_debug)
            fprintf(stderr, "lvt: WPF TAP DLL not found: %ls\n", tapDll.c_str());
        return false;
    }

    // Check managed assembly is alongside
    std::wstring managedDll = exeDir + L"\\LvtWpfTap.dll";
    if (GetFileAttributesW(managedDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (g_debug)
            fprintf(stderr, "lvt: WPF managed assembly not found: %ls\n", managedDll.c_str());
        return false;
    }

    std::wstring pipeName = make_pipe_name();

    // Write pipe name to sidecar file for the TAP DLL to read
    if (!write_pipe_name_file(exeDir, pipeName)) {
        fprintf(stderr, "lvt: failed to write pipe name file\n");
        return false;
    }

    // Create named pipe with AppContainer-accessible DACL
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR rawSd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1, &rawSd, nullptr);
    wil::unique_hlocal securityDescriptor(rawSd);
    sa.lpSecurityDescriptor = securityDescriptor.get();

    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 1024 * 1024, 10000, &sa));

    if (!pipe) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return false;
    }

    // Start overlapped connect before injection
    OVERLAPPED ov = {};
    wil::unique_event connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ov.hEvent = connectEvent.get();
    ConnectNamedPipe(pipe.get(), &ov);
    DWORD connectErr = GetLastError();

    // Inject TAP DLL. Since the TAP DLL calls FreeLibraryAndExitThread after
    // collection, it unloads itself, so each run is a fresh injection.
    if (!inject_dll(pid, tapDll)) {
        CancelIo(pipe.get());
        return false;
    }

    if (g_debug)
        fprintf(stderr, "lvt: WPF injection succeeded, waiting for tree data...\n");

    // Wait for connection
    if (connectErr == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 15000) != WAIT_OBJECT_0) {
            fprintf(stderr, "lvt: WPF TAP DLL did not connect (timeout)\n");
            CancelIo(pipe.get());
            return false;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        fprintf(stderr, "lvt: WPF ConnectNamedPipe failed (error %lu)\n", connectErr);
        return false;
    }

    // Read all data
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
                if (WaitForSingleObject(readOv.hEvent, 15000) != WAIT_OBJECT_0) {
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

    // Clean up sidecar file
    DeleteFileW((exeDir + L"\\lvt_wpf_pipe.txt").c_str());

    if (g_debug)
        fprintf(stderr, "lvt: received %zu bytes of WPF tree data\n", data.size());

    if (data.empty()) {
        if (g_debug)
            fprintf(stderr, "lvt: no WPF tree data received\n");
        return false;
    }

    // Graft WPF elements. The JSON is an array of Window roots, each mapping
    // to an HwndWrapper HWND in the Win32 tree.
    auto parsed = wpf_parse_tree_json(data, "wpf");
    for (auto& el : parsed) {
        root.children.push_back(std::move(el));
    }

    return true;
}

} // namespace lvt
