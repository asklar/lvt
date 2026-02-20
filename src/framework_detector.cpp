#include "framework_detector.h"
#include <wil/resource.h>
#include <Psapi.h>

#pragma comment(lib, "version.lib")

namespace lvt {

std::string framework_to_string(Framework f) {
    switch (f) {
    case Framework::Win32:  return "win32";
    case Framework::ComCtl: return "comctl";
    case Framework::Xaml:   return "xaml";
    case Framework::WinUI3: return "winui3";
    }
    return "unknown";
}

static const wchar_t* comctl_classes[] = {
    L"SysListView32", L"SysTreeView32", L"SysTabControl32",
    L"msctls_statusbar32", L"ToolbarWindow32", L"msctls_trackbar32",
    L"SysHeader32", L"msctls_progress32", L"SysAnimate32",
    L"SysDateTimePick32", L"SysMonthCal32", L"ReBarWindow32",
    L"tooltips_class32", L"SysPager", L"SysLink",
};

struct DetectData {
    bool hasComCtl = false;
    bool hasWinUI3 = false;
    bool hasXaml = false;
};

static BOOL CALLBACK detect_child_proc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<DetectData*>(lParam);
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);

    for (auto* cc : comctl_classes) {
        if (_wcsicmp(cls, cc) == 0) {
            data->hasComCtl = true;
            break;
        }
    }

    if (wcsstr(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") ||
        wcsstr(cls, L"Microsoft.UI.") ||
        _wcsicmp(cls, L"WinUIDesktopWin32WindowClass") == 0 ||
        _wcsicmp(cls, L"InputNonClientPointerSource") == 0) {
        data->hasWinUI3 = true;
    }

    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        data->hasXaml = true;
    }

    return TRUE;
}

// Get the full path of a module loaded in a remote process.
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

// Extract file version string from a DLL path (e.g. "3.1.7.2602").
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

    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             HIWORD(fileInfo->dwFileVersionMS), LOWORD(fileInfo->dwFileVersionMS),
             HIWORD(fileInfo->dwFileVersionLS), LOWORD(fileInfo->dwFileVersionLS));
    return buf;
}

struct ModuleDetection {
    bool found = false;
    std::string version;
};

static ModuleDetection detect_module(DWORD pid, const wchar_t* moduleName) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!proc) return {};

    auto path = get_module_path(proc.get(), moduleName);
    if (path.empty()) return {};

    return {true, get_file_version(path)};
}

std::vector<FrameworkInfo> detect_frameworks(HWND hwnd, DWORD pid) {
    std::vector<FrameworkInfo> result;
    result.push_back({Framework::Win32, {}});

    DetectData data;
    if (hwnd) {
        EnumChildWindows(hwnd, detect_child_proc, reinterpret_cast<LPARAM>(&data));
        if (data.hasComCtl) {
            std::string comctlVer;
            if (pid) {
                auto det = detect_module(pid, L"comctl32.dll");
                if (det.found) comctlVer = det.version;
            }
            result.push_back({Framework::ComCtl, comctlVer});
        }
    }

    bool detectedXaml = false;
    if (pid) {
        auto winui = detect_module(pid, L"Microsoft.UI.Xaml.dll");
        if (winui.found) {
            result.push_back({Framework::WinUI3, winui.version});
            detectedXaml = true;
        } else {
            auto xaml = detect_module(pid, L"Windows.UI.Xaml.dll");
            if (xaml.found) {
                result.push_back({Framework::Xaml, xaml.version});
                detectedXaml = true;
            }
        }
    }

    // Class-name fallback (works when module enumeration fails)
    if (!detectedXaml) {
        if (data.hasWinUI3)
            result.push_back({Framework::WinUI3, {}});
        else if (data.hasXaml)
            result.push_back({Framework::Xaml, {}});
    }

    return result;
}

} // namespace lvt
