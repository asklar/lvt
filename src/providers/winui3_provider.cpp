#include "winui3_provider.h"
#include "xaml_diag_common.h"
#include <cstdio>
#include <Windows.h>
#include <Psapi.h>
#include <string>

namespace lvt {

// Label DesktopChildSiteBridge and related WinUI3 host windows
static void label_winui3_windows(Element& el) {
    if (el.className == "Microsoft.UI.Content.DesktopChildSiteBridge") {
        el.framework = "winui3";
        el.type = "DesktopChildSiteBridge";
    } else if (el.className == "InputNonClientPointerSource") {
        el.framework = "winui3";
        el.type = "InputNonClientPointerSource";
    } else if (el.className == "InputSiteWindowClass") {
        el.framework = "winui3";
        el.type = "InputSite";
    }
    for (auto& child : el.children) {
        label_winui3_windows(child);
    }
}

// Find the FrameworkUdk.dll path loaded in the target process
static std::wstring find_framework_udk(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return {};

    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
        CloseHandle(proc);
        return {};
    }

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc, modules[i], name, MAX_PATH)) {
            if (_wcsicmp(name, L"Microsoft.Internal.FrameworkUdk.dll") == 0) {
                wchar_t fullPath[MAX_PATH]{};
                GetModuleFileNameExW(proc, modules[i], fullPath, MAX_PATH);
                CloseHandle(proc);
                return fullPath;
            }
        }
    }
    CloseHandle(proc);
    return {};
}

void WinUI3Provider::enrich(Element& root, HWND hwnd, DWORD pid) {
    label_winui3_windows(root);

    // Try XAML diagnostics injection for the full visual tree
    // WinUI3 registers "WinUIVisualDiagConnection" endpoints
    // InitializeXamlDiagnosticsEx can be loaded from FrameworkUdk.dll (WinAppSDK)
    // or from Windows.UI.Xaml.dll (System32)
    std::wstring frameworkUdk = find_framework_udk(pid);
    std::wstring initDll;
    if (!frameworkUdk.empty()) {
        initDll = frameworkUdk;
    } else {
        // Fall back to system XAML
        initDll = L"Windows.UI.Xaml.dll";
    }

    inject_and_collect_xaml_tree(root, hwnd, pid, L"", initDll, "winui3");
}

} // namespace lvt
