#include "winui3_provider.h"
#include <cstdio>
#include <Windows.h>
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

void WinUI3Provider::enrich(Element& root, HWND /*hwnd*/, DWORD /*pid*/) {
    label_winui3_windows(root);
    // TODO: Full XAML visual tree inspection via InitializeXamlDiagnosticsEx +
    // TAP DLL injection. Requires careful cross-process COM marshaling and
    // matching the target's WinAppSDK version. See src/tap/ for the TAP DLL
    // implementation (not yet safe for production use).
}

} // namespace lvt
