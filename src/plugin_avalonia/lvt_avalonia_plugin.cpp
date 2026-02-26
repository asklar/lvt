// lvt_avalonia_plugin.cpp — LVT plugin for Avalonia UI framework support.
// Detects Avalonia apps by checking for Avalonia.Base.dll in the target process,
// injects the Avalonia TAP DLL, and reads the visual tree JSON over a named pipe.

#include "plugin.h"

#include <Windows.h>
#include <Psapi.h>
#include <objbase.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>

#pragma comment(lib, "Psapi.lib")

// ---------- Logging ----------

static bool g_debug = false;

static void DebugLog(const char* fmt, ...) {
    if (!g_debug) return;
    fprintf(stderr, "lvt-avalonia: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

// ---------- Plugin metadata ----------

static LvtPluginInfo s_info = {
    sizeof(LvtPluginInfo),
    LVT_PLUGIN_API_VERSION,
    "avalonia",
    "Avalonia UI framework visual tree support"
};

// ---------- Module detection helpers ----------

static std::wstring get_module_path(HANDLE proc, const wchar_t* moduleName) {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return {};

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc, modules[i], name, MAX_PATH)) {
            if (_wcsicmp(name, moduleName) == 0) {
                wchar_t fullPath[MAX_PATH]{};
                if (GetModuleFileNameExW(proc, modules[i], fullPath, MAX_PATH))
                    return fullPath;
                return {};
            }
        }
    }
    return {};
}

static std::string get_file_version(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD verHandle = 0;
    DWORD verSize = GetFileVersionInfoSizeW(path.c_str(), &verHandle);
    if (verSize == 0) return {};

    std::vector<BYTE> verData(verSize);
    if (!GetFileVersionInfoW(path.c_str(), verHandle, verSize, verData.data()))
        return {};

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(verData.data(), L"\\", reinterpret_cast<void**>(&fileInfo), &len))
        return {};

    DWORD ms = fileInfo->dwProductVersionMS;
    DWORD ls = fileInfo->dwProductVersionLS;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
    return buf;
}

// ---------- DLL path helpers ----------

static std::wstring get_plugin_dir() {
    wchar_t path[MAX_PATH];
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&s_info), &hm);
    GetModuleFileNameW(hm, path, MAX_PATH);
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
}

static std::wstring make_pipe_name() {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t buf[80];
    swprintf_s(buf, L"\\\\.\\pipe\\lvt_avl_%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

// ---------- Injection ----------

static bool write_pipe_name_file(const std::wstring& dir, const std::wstring& pipeName) {
    std::wstring path = dir + L"\\lvt_avalonia_pipe.txt";
    int len = WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, utf8.data(), len, nullptr, nullptr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8.data(), utf8.size());
    return true;
}

static bool inject_dll(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!proc) {
        DebugLog("failed to open target process %lu (error %lu)", pid, GetLastError());
        return false;
    }

    size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(proc, nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) {
        DebugLog("VirtualAllocEx failed (error %lu)", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remoteMem, dllPath.c_str(), pathBytes, nullptr)) {
        DebugLog("WriteProcessMemory failed (error %lu)", GetLastError());
        VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(
        proc, nullptr, 0, loadLibAddr, remoteMem, 0, nullptr);
    if (!thread) {
        DebugLog("CreateRemoteThread failed (error %lu)", GetLastError());
        VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exitCode == 0) {
        DebugLog("LoadLibraryW failed in target process");
        return false;
    }

    DebugLog("TAP DLL injected into pid %lu", pid);
    return true;
}

// ---------- Version string storage ----------
static char s_version_buf[64];

// ---------- Plugin exports ----------

extern "C" {

__declspec(dllexport) LvtPluginInfo* lvt_plugin_info(void) {
    // Check for debug env var
    char dbg[8]{};
    if (GetEnvironmentVariableA("LVT_DEBUG", dbg, sizeof(dbg)) > 0)
        g_debug = true;
    return &s_info;
}

__declspec(dllexport) int lvt_detect_framework(DWORD pid, HWND /*hwnd*/, LvtFrameworkDetection* out) {
    if (!out) return 0;

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 0;

    auto avaloniaPath = get_module_path(proc, L"Avalonia.Base.dll");
    if (avaloniaPath.empty()) {
        // Try alternative: some older versions may just be "Avalonia.dll"
        avaloniaPath = get_module_path(proc, L"Avalonia.dll");
    }
    CloseHandle(proc);

    if (avaloniaPath.empty()) return 0;

    auto version = get_file_version(avaloniaPath);
    if (!version.empty()) {
        strncpy_s(s_version_buf, version.c_str(), sizeof(s_version_buf) - 1);
        out->version = s_version_buf;
    }

    out->struct_size = sizeof(LvtFrameworkDetection);
    out->name = "avalonia";
    return 1;
}

__declspec(dllexport) int lvt_enrich_tree(HWND hwnd, DWORD pid,
                                           const char* /*element_class_filter*/,
                                           char** json_out)
{
    if (!json_out) return 0;
    *json_out = nullptr;

    // Locate the TAP DLL and managed assembly in the "avalonia" subdirectory
    std::wstring pluginDir = get_plugin_dir();
    std::wstring tapDir = pluginDir + L"\\avalonia";

#if defined(_M_ARM64)
    std::wstring tapDll = tapDir + L"\\lvt_avalonia_tap_arm64.dll";
#else
    std::wstring tapDll = tapDir + L"\\lvt_avalonia_tap_x64.dll";
#endif

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DebugLog("TAP DLL not found: %ls", tapDll.c_str());
        return 0;
    }

    std::wstring managedDll = tapDir + L"\\LvtAvaloniaTreeWalker.dll";
    if (GetFileAttributesW(managedDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DebugLog("Managed assembly not found: %ls", managedDll.c_str());
        return 0;
    }

    std::wstring pipeName = make_pipe_name();

    // Write pipe name to sidecar file in the TAP directory
    if (!write_pipe_name_file(tapDir, pipeName)) {
        DebugLog("failed to write pipe name file");
        return 0;
    }

    // Create named pipe
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
        DebugLog("failed to create named pipe (error %lu)", GetLastError());
        return 0;
    }

    // Start overlapped connect before injection
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(pipe, &ov);
    DWORD connectErr = GetLastError();

    // Inject TAP DLL
    if (!inject_dll(pid, tapDll)) {
        CancelIo(pipe);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        return 0;
    }

    DebugLog("injection succeeded, waiting for tree data...");

    // Wait for connection
    if (connectErr == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 15000) != WAIT_OBJECT_0) {
            DebugLog("TAP DLL did not connect (timeout)");
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return 0;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        DebugLog("ConnectNamedPipe failed (error %lu)", connectErr);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        return 0;
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
    DeleteFileW((tapDir + L"\\lvt_avalonia_pipe.txt").c_str());

    DebugLog("received %zu bytes of tree data", data.size());

    if (data.empty()) {
        DebugLog("no tree data received");
        return 0;
    }

    // Return a malloc'd copy (caller frees with lvt_plugin_free)
    char* result = static_cast<char*>(malloc(data.size() + 1));
    if (!result) return 0;
    memcpy(result, data.c_str(), data.size() + 1);
    *json_out = result;
    return 1;
}

__declspec(dllexport) void lvt_plugin_free(void* ptr) {
    free(ptr);
}

} // extern "C"
