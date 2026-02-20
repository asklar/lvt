#include "target.h"
#include <wil/resource.h>
#include <Psapi.h>
#include <vector>
#include <algorithm>

namespace lvt {

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
        int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        std::string s(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), sz, nullptr, nullptr);
        return s;
    }
    return {};
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

        // Pick the largest visible window as the main window
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
    }
    return info;
}

} // namespace lvt
