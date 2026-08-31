#include "winui3_provider.h"
#include "xaml_diag_common.h"
#include <cstdio>
#include <Windows.h>
#include <Psapi.h>
#include <wil/resource.h>
#include <string>

namespace lvt {

// Label DesktopChildSiteBridge and related WinUI3 host windows
static void label_winui3_windows(Element& el) {
    if (el.className == "Microsoft.UI.Content.DesktopChildSiteBridge") {
        el.framework = "winui3";
        el.type = "DesktopChildSiteBridge";
        el.providerHandle = 0;
    } else if (el.className == "InputNonClientPointerSource") {
        el.framework = "winui3";
        el.type = "InputNonClientPointerSource";
        el.providerHandle = 0;
    } else if (el.className == "InputSiteWindowClass") {
        el.framework = "winui3";
        el.type = "InputSite";
        el.providerHandle = 0;
    }
    for (auto& child : el.children) {
        label_winui3_windows(child);
    }
}

static bool has_winui3_host(const Element& el) {
    if (el.className ==
        "Microsoft.UI.Content.DesktopChildSiteBridge") {
        return true;
    }
    for (const auto& child : el.children) {
        if (has_winui3_host(child))
            return true;
    }
    return false;
}

// Find the FrameworkUdk.dll path loaded in the target process
static std::wstring find_framework_udk(DWORD pid) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!proc) return {};

    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc.get(), modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
        return {};
    }

    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc.get(), modules[i], name, MAX_PATH)) {
            if (_wcsicmp(name, L"Microsoft.Internal.FrameworkUdk.dll") == 0) {
                wchar_t fullPath[MAX_PATH]{};
                GetModuleFileNameExW(proc.get(), modules[i], fullPath, MAX_PATH);
                return fullPath;
            }
        }
    }
    return {};
}

bool WinUI3Provider::enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties) {
    label_winui3_windows(root);
    if (!has_winui3_host(root))
        return true;

    // Try XAML diagnostics injection for the full visual tree
    // WinUI3 registers "WinUIVisualDiagConnection" endpoints
    // InitializeXamlDiagnosticsEx for WinUI 3 is exported by the Windows App
    // SDK's FrameworkUdk. Microsoft.UI.Xaml.dll can also be the WinUI 2
    // controls library hosted by system XAML, so falling back to
    // Windows.UI.Xaml.dll here would use the wrong endpoint flavor.
    std::wstring frameworkUdk = find_framework_udk(pid);
    if (frameworkUdk.empty())
        return false;

    return inject_and_collect_xaml_tree(
        root, hwnd, pid, L"", frameworkUdk,
        "winui3", L"WinUIVisualDiagConnection",
        fastProperties);
}

std::shared_ptr<IFrameworkConnection> WinUI3Provider::open_connection(HWND hwnd, DWORD pid) {
    std::wstring frameworkUdk = find_framework_udk(pid);
    if (frameworkUdk.empty())
        return nullptr;
    return make_xaml_diag_connection(hwnd, pid, L"", frameworkUdk, "winui3",
                                     L"WinUIVisualDiagConnection");
}

bool WinUI3Provider::enrich_with_connection(Element& root, IFrameworkConnection& connection, bool fastProperties) {
    label_winui3_windows(root);
    if (!has_winui3_host(root))
        return true;
    return connection.get_tree(
        root, fastProperties);
}

} // namespace lvt
