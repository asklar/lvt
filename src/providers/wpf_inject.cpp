// wpf_inject.cpp — WPF DLL injection and tree collection.
// Injects lvt_wpf_tap_x64.dll into the target process via
// CreateRemoteThread + LoadLibraryW, then reads the WPF visual tree
// JSON over a named pipe.

#include "wpf_inject.h"
#include "../debug.h"
#include "../target.h"

#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <aclapi.h>
#include <Psapi.h>
#include <wil/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cmath>
#include <climits>
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

static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
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

// Safe double-to-int conversion: clamp to int range and reject non-finite values.
static int safe_double_to_int(double v) {
    if (!std::isfinite(v)) return 0;
    if (v >= static_cast<double>(INT_MAX)) return INT_MAX;
    if (v <= static_cast<double>(INT_MIN)) return INT_MIN;
    return static_cast<int>(v);
}

// Recursively graft JSON tree nodes into an Element tree.
static void graft_json_node(const json& j, Element& parent, const std::string& framework) {
    Element el;
    el.framework = framework;
    el.className = sanitize(j.value("type", ""));

    // Simplify type name: "System.Windows.Controls.Button" -> "Button"
    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    el.text = sanitize(j.value("text", ""));
    if (el.text.empty())
        el.text = sanitize(j.value("name", ""));

    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    if (w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h)
        && std::isfinite(ox) && std::isfinite(oy)) {
        el.bounds.x = safe_double_to_int(ox);
        el.bounds.y = safe_double_to_int(oy);
        el.bounds.width = safe_double_to_int(w);
        el.bounds.height = safe_double_to_int(h);
    }

    // Visibility/enabled as properties
    if (j.contains("visible") && j["visible"].is_boolean() && !j["visible"].get<bool>())
        el.properties["visible"] = "false";
    if (j.contains("enabled") && j["enabled"].is_boolean() && !j["enabled"].get<bool>())
        el.properties["enabled"] = "false";

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_json_node(child, el, framework);
        }
    }

    parent.children.push_back(std::move(el));
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

    std::wstring exeDir = get_exe_dir();
    const wchar_t* tapSuffix = (get_host_architecture() == Architecture::arm64)
        ? L"\\lvt_wpf_tap_arm64.dll" : (sizeof(void*) == 4)
        ? L"\\lvt_wpf_tap_x86.dll" : L"\\lvt_wpf_tap_x64.dll";
    std::wstring tapDll = exeDir + tapSuffix;

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
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 1024 * 1024, 10000, &sa);
    LocalFree(sa.lpSecurityDescriptor);

    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "lvt: failed to create named pipe (error %lu)\n", GetLastError());
        return false;
    }

    // Start overlapped connect before injection
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(pipe, &ov);
    DWORD connectErr = GetLastError();

    // Inject TAP DLL. Since the TAP DLL calls FreeLibraryAndExitThread after
    // collection, it unloads itself, so each run is a fresh injection.
    if (!inject_dll(pid, tapDll)) {
        CancelIo(pipe);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        return false;
    }

    if (g_debug)
        fprintf(stderr, "lvt: WPF injection succeeded, waiting for tree data...\n");

    // Wait for connection
    if (connectErr == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 15000) != WAIT_OBJECT_0) {
            fprintf(stderr, "lvt: WPF TAP DLL did not connect (timeout)\n");
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return false;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        fprintf(stderr, "lvt: WPF ConnectNamedPipe failed (error %lu)\n", connectErr);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        return false;
    }
    CloseHandle(ov.hEvent);

    // Read all data
    std::string data;
    char buf[4096];
    DWORD bytesRead = 0;
    OVERLAPPED readOv = {};
    readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        ResetEvent(readOv.hEvent);
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), &bytesRead, &readOv);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(readOv.hEvent, 15000) != WAIT_OBJECT_0) {
                    CancelIo(pipe);
                    break;
                }
                if (!GetOverlappedResult(pipe, &readOv, &bytesRead, FALSE) || bytesRead == 0)
                    break;
            } else {
                break;
            }
        } else if (bytesRead == 0) {
            break;
        }
        data.append(buf, bytesRead);
    }
    CloseHandle(readOv.hEvent);
    CloseHandle(pipe);

    // Clean up sidecar file
    DeleteFileW((exeDir + L"\\lvt_wpf_pipe.txt").c_str());

    if (g_debug)
        fprintf(stderr, "lvt: received %zu bytes of WPF tree data\n", data.size());

    if (data.empty()) {
        if (g_debug)
            fprintf(stderr, "lvt: no WPF tree data received\n");
        return false;
    }

    json treeJson;
    try {
        treeJson = json::parse(data);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "lvt: failed to parse WPF tree JSON: %s\n", e.what());
        return false;
    }

    // Graft WPF elements. The JSON is an array of Window roots.
    // Each maps to an HwndWrapper HWND in the Win32 tree.
    if (treeJson.is_array()) {
        for (auto& node : treeJson) {
            graft_json_node(node, root, "wpf");
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, "wpf");
    }

    return true;
}

} // namespace lvt
