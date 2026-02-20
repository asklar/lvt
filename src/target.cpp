#include "target.h"
#include <wil/resource.h>
#include <Psapi.h>
#include <vector>
#include <algorithm>

namespace lvt {

const char* architecture_name(Architecture arch) {
    switch (arch) {
    case Architecture::x64:   return "x64";
    case Architecture::arm64: return "arm64";
    default:                  return "unknown";
    }
}

Architecture get_host_architecture() {
#if defined(_M_ARM64)
    return Architecture::arm64;
#elif defined(_M_X64)
    return Architecture::x64;
#else
    return Architecture::unknown;
#endif
}

Architecture detect_process_architecture(DWORD pid) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return get_host_architecture();

    // IsWow64Process2 is available on Win10 1709+
    auto fnIsWow64Process2 = reinterpret_cast<decltype(&IsWow64Process2)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (fnIsWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (fnIsWow64Process2(proc.get(), &processMachine, &nativeMachine)) {
            // processMachine == IMAGE_FILE_MACHINE_UNKNOWN means the process is native
            USHORT actual = (processMachine != IMAGE_FILE_MACHINE_UNKNOWN)
                            ? processMachine : nativeMachine;
            switch (actual) {
            case IMAGE_FILE_MACHINE_AMD64: return Architecture::x64;
            case IMAGE_FILE_MACHINE_ARM64: return Architecture::arm64;
            }
        }
    }

    // Fallback: assume same as host
    return get_host_architecture();
}

static std::string wstr_to_utf8(const wchar_t* ws, int len = -1) {
    if (!ws) return {};
    if (len < 0) len = static_cast<int>(wcslen(ws));
    if (len == 0) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len, s.data(), sz, nullptr, nullptr);
    return s;
}

struct EnumData {
    DWORD pid;
    std::vector<HWND> candidates;
};

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumData*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == data->pid && IsWindowVisible(hwnd)) {
        data->candidates.push_back(hwnd);
    }
    return TRUE;
}

static std::string get_process_name(DWORD pid) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(proc.get(), 0, path, &size)) {
        std::wstring ws(path, size);
        auto pos = ws.find_last_of(L"\\/");
        if (pos != std::wstring::npos) ws = ws.substr(pos + 1);
        return wstr_to_utf8(ws.c_str(), static_cast<int>(ws.size()));
    }
    return {};
}

static std::string get_window_title(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return {};
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(hwnd, buf.data(), len + 1);
    return wstr_to_utf8(buf.c_str(), len);
}

// Case-insensitive substring match on ASCII/UTF-8 strings.
static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) { return tolower(static_cast<unsigned char>(a)) ==
                                                      tolower(static_cast<unsigned char>(b)); });
    return it != haystack.end();
}

TargetInfo resolve_target(HWND hwnd, DWORD pid) {
    TargetInfo info;

    if (hwnd) {
        info.hwnd = hwnd;
        GetWindowThreadProcessId(hwnd, &info.pid);
    } else if (pid) {
        info.pid = pid;
        EnumData data{pid, {}};
        EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&data));

        HWND best = nullptr;
        int bestArea = 0;
        for (auto h : data.candidates) {
            RECT rc{};
            GetWindowRect(h, &rc);
            int area = (rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > bestArea) {
                bestArea = area;
                best = h;
            }
        }
        info.hwnd = best;
    }

    if (info.pid == 0 && info.hwnd) {
        GetWindowThreadProcessId(info.hwnd, &info.pid);
    }
    if (info.pid) {
        info.processName = get_process_name(info.pid);
        info.architecture = detect_process_architecture(info.pid);
    }
    return info;
}

struct EnumAllData {
    std::vector<WindowMatch> matches;
};

static BOOL CALLBACK enum_all_windows_proc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumAllData*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    WindowMatch m;
    m.hwnd = hwnd;
    m.pid = pid;
    m.processName = get_process_name(pid);
    m.windowTitle = get_window_title(hwnd);
    data->matches.push_back(std::move(m));
    return TRUE;
}

std::vector<WindowMatch> find_by_process_name(const std::string& name) {
    EnumAllData data;
    EnumWindows(enum_all_windows_proc, reinterpret_cast<LPARAM>(&data));

    std::vector<WindowMatch> results;
    for (auto& m : data.matches) {
        if (icontains(m.processName, name)) {
            results.push_back(std::move(m));
        }
    }
    return results;
}

std::vector<WindowMatch> find_by_title(const std::string& title) {
    EnumAllData data;
    EnumWindows(enum_all_windows_proc, reinterpret_cast<LPARAM>(&data));

    std::vector<WindowMatch> results;
    for (auto& m : data.matches) {
        if (icontains(m.windowTitle, title)) {
            results.push_back(std::move(m));
        }
    }
    return results;
}

} // namespace lvt
