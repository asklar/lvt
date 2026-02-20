#include "xaml_provider.h"
#include <cstdio>
#include <Windows.h>

namespace lvt {

// Mark CoreWindow children as XAML content without injecting into the target
static void label_xaml_windows(Element& el) {
    // Windows.UI.Core.CoreWindow is the host for UWP XAML content
    if (el.className == "Windows.UI.Core.CoreWindow") {
        el.framework = "xaml";
        el.type = "CoreWindow";
    }
    for (auto& child : el.children) {
        label_xaml_windows(child);
    }
}

void XamlProvider::enrich(Element& root, HWND /*hwnd*/, DWORD /*pid*/) {
    label_xaml_windows(root);
}

} // namespace lvt
