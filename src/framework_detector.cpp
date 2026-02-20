#include "framework_detector.h"
#include <wil/resource.h>
#include <Psapi.h>

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

    // WinUI 3 signature class names
    if (wcsstr(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") ||
        wcsstr(cls, L"Microsoft.UI.") ||
        _wcsicmp(cls, L"WinUIDesktopWin32WindowClass") == 0 ||
        _wcsicmp(cls, L"InputNonClientPointerSource") == 0) {
        data->hasWinUI3 = true;
    }

    // UWP XAML signature
    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        data->hasXaml = true;
    }

    return TRUE;
}

static bool process_has_module(DWORD pid, const wchar_t* moduleName) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!proc) return false;

    HMODULE modules[1024];
    DWORD needed = 0;
    // LIST_MODULES_ALL to handle cross-bitness (x86 process inspecting x64 target)
    if (!EnumProcessModulesEx(proc.get(), modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return false;

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc.get(), modules[i], name, MAX_PATH)) {
            if (_wcsicmp(name, moduleName) == 0)
                return true;
        }
    }
    return false;
}

std::vector<Framework> detect_frameworks(HWND hwnd, DWORD pid) {
    std::vector<Framework> result;
    result.push_back(Framework::Win32);

    DetectData data;
    if (hwnd) {
        EnumChildWindows(hwnd, detect_child_proc, reinterpret_cast<LPARAM>(&data));
        if (data.hasComCtl)
            result.push_back(Framework::ComCtl);
    }

    // Try module-based detection first, fall back to class-name heuristics
    bool detectedXaml = false;
    if (pid) {
        if (process_has_module(pid, L"Microsoft.UI.Xaml.dll")) {
            result.push_back(Framework::WinUI3);
            detectedXaml = true;
        } else if (process_has_module(pid, L"Windows.UI.Xaml.dll")) {
            result.push_back(Framework::Xaml);
            detectedXaml = true;
        }
    }

    // Class-name fallback (works cross-bitness where EnumProcessModulesEx may fail)
    if (!detectedXaml) {
        if (data.hasWinUI3)
            result.push_back(Framework::WinUI3);
        else if (data.hasXaml)
            result.push_back(Framework::Xaml);
    }

    return result;
}

} // namespace lvt
