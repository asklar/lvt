// lvt_chromium_plugin.cpp — LVT plugin for Chrome/Edge DOM tree inspection.
// Detects Chromium-based browsers by checking for chrome.dll or msedge.dll,
// then communicates with the LVT Chromium extension via a native messaging host
// relay to retrieve the DOM tree.

#include "plugin.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")

using json = nlohmann::json;

// ---------- Logging ----------

static bool g_debug = false;

static void DebugLog(const char* fmt, ...) {
    if (!g_debug) return;
    fprintf(stderr, "lvt-chromium: ");
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
    "chromium",
    "Chrome/Edge DOM tree support via browser extension"
};

// ---------- Module detection helpers ----------

static bool has_module(HANDLE proc, const wchar_t* moduleName) {
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return false;

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc, modules[i], name, MAX_PATH)) {
            if (_wcsicmp(name, moduleName) == 0)
                return true;
        }
    }
    return false;
}

static std::string get_module_version(HANDLE proc, const wchar_t* moduleName) {
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return {};

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (!GetModuleBaseNameW(proc, modules[i], name, MAX_PATH))
            continue;
        if (_wcsicmp(name, moduleName) != 0)
            continue;

        wchar_t fullPath[MAX_PATH]{};
        if (!GetModuleFileNameExW(proc, modules[i], fullPath, MAX_PATH))
            return {};

        DWORD verHandle = 0;
        DWORD verSize = GetFileVersionInfoSizeW(fullPath, &verHandle);
        if (verSize == 0) return {};

        std::vector<BYTE> verData(verSize);
        if (!GetFileVersionInfoW(fullPath, verHandle, verSize, verData.data()))
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
    return {};
}

// ---------- Named pipe communication ----------

static const char* PIPE_NAME = "\\\\.\\pipe\\lvt_chromium";

// Write a length-prefixed message to a pipe
static bool write_pipe_message(HANDLE pipe, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    DWORD bytesWritten = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL ok = WriteFile(pipe, &len, 4, &bytesWritten, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 5000);
        if (!GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    if (bytesWritten != 4) { CloseHandle(ov.hEvent); return false; }

    ResetEvent(ov.hEvent);
    ok = WriteFile(pipe, msg.data(), len, &bytesWritten, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 30000);
        if (!GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }

    CloseHandle(ov.hEvent);
    return bytesWritten == len;
}

// Read a length-prefixed message from a pipe
static bool read_pipe_message(HANDLE pipe, std::string& out, DWORD timeoutMs = 30000) {
    uint32_t len = 0;
    DWORD bytesRead = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL ok = ReadFile(pipe, &len, 4, &bytesRead, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            return false;
        }
        if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);

    if (bytesRead != 4 || len == 0 || len > 64 * 1024 * 1024) // 64MB max for large DOMs
        return false;

    out.resize(len);
    DWORD totalRead = 0;
    while (totalRead < len) {
        ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ok = ReadFile(pipe, out.data() + totalRead, len - totalRead, &bytesRead, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        if (bytesRead == 0) return false;
        totalRead += bytesRead;
    }
    return true;
}

// ---------- Version string storage ----------
static char s_version_buf[64];
static char s_browser_name[32];

// ---------- Plugin exports ----------

extern "C" {

__declspec(dllexport) LvtPluginInfo* lvt_plugin_info(void) {
    char dbg[8]{};
    if (GetEnvironmentVariableA("LVT_DEBUG", dbg, sizeof(dbg)) > 0)
        g_debug = true;
    return &s_info;
}

__declspec(dllexport) int lvt_detect_framework(DWORD pid, HWND /*hwnd*/, LvtFrameworkDetection* out) {
    if (!out) return 0;

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 0;

    // Check for Chrome or Edge
    bool isChrome = has_module(proc, L"chrome.dll");
    bool isEdge = has_module(proc, L"msedge.dll");

    if (!isChrome && !isEdge) {
        CloseHandle(proc);
        return 0;
    }

    const wchar_t* versionModule = isEdge ? L"msedge.dll" : L"chrome.dll";
    auto version = get_module_version(proc, versionModule);
    CloseHandle(proc);

    if (!version.empty()) {
        // Include browser name in version for display: "145.0.3800.70 (Edge)"
        auto fullVersion = version + (isEdge ? " (Edge)" : " (Chrome)");
        strncpy_s(s_version_buf, fullVersion.c_str(), sizeof(s_version_buf) - 1);
        out->version = s_version_buf;
    }

    // Name must match plugin info name ("chromium") for tree builder lookup
    out->struct_size = sizeof(LvtFrameworkDetection);
    out->name = "chromium";

    DebugLog("detected %s %s", isEdge ? "Edge" : "Chrome", version.c_str());
    return 1;
}

__declspec(dllexport) int lvt_enrich_tree(HWND /*hwnd*/, DWORD /*pid*/,
                                           const char* /*element_class_filter*/,
                                           char** json_out)
{
    if (!json_out) return 0;
    *json_out = nullptr;

    // Connect to the native messaging host's named pipe
    HANDLE pipe = CreateFileA(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        DebugLog("failed to connect to native messaging host pipe (error %lu). "
                 "Is the LVT Chromium extension installed and active?", GetLastError());
        fprintf(stderr, "lvt-chromium: Cannot connect to browser extension.\n"
                        "  Ensure the LVT extension is installed in Chrome/Edge and\n"
                        "  the native messaging host is registered (lvt_chromium_host.exe --register).\n");
        return 0;
    }

    DebugLog("connected to native messaging host pipe");

    // Send getDOM request
    std::string request = "{\"type\":\"getDOM\",\"requestId\":\"1\",\"tabId\":\"active\"}";
    if (!write_pipe_message(pipe, request)) {
        DebugLog("failed to send getDOM request");
        CloseHandle(pipe);
        return 0;
    }

    DebugLog("sent getDOM request, waiting for response...");

    // Read response (may be large — full DOM tree)
    std::string response;
    if (!read_pipe_message(pipe, response, 60000)) {
        DebugLog("failed to read DOM response (timeout or error)");
        CloseHandle(pipe);
        return 0;
    }

    CloseHandle(pipe);

    DebugLog("received %zu bytes of DOM data", response.size());

    if (response.empty()) {
        DebugLog("empty DOM response");
        return 0;
    }

    // Parse the response envelope and extract the "tree" field.
    // The extension returns: {"type":"domTree","tree":[...],...}
    // The plugin loader expects a JSON array of element nodes.
    try {
        auto envelope = json::parse(response);

        if (envelope.contains("type") && envelope["type"] == "error") {
            auto msg = envelope.value("message", "unknown error");
            DebugLog("extension returned error: %s", msg.c_str());
            fprintf(stderr, "lvt-chromium: %s\n", msg.c_str());
            return 0;
        }

        json tree;
        if (envelope.contains("tree") && envelope["tree"].is_array()) {
            tree = envelope["tree"];
        } else if (envelope.is_array()) {
            tree = envelope;
        } else {
            DebugLog("unexpected response format");
            return 0;
        }

        auto treeStr = tree.dump();
        char* result = static_cast<char*>(malloc(treeStr.size() + 1));
        if (!result) return 0;
        memcpy(result, treeStr.c_str(), treeStr.size() + 1);
        *json_out = result;
        return 1;
    } catch (const json::parse_error& e) {
        DebugLog("failed to parse response JSON: %s", e.what());
        return 0;
    }
}

__declspec(dllexport) void lvt_plugin_free(void* ptr) {
    free(ptr);
}

} // extern "C"
