#include "xaml_provider.h"
#include "xaml_diag_common.h"
#include <cstdio>
#include <functional>
#include <Windows.h>

namespace lvt {

void XamlProvider::enrich(Element& root, HWND hwnd, DWORD pid) {
    bool hasCoreWindow = false;
    std::function<void(Element&)> findCoreWindow = [&](Element& el) {
        if (el.className == "Windows.UI.Core.CoreWindow") {
            el.framework = "xaml";
            el.type = "CoreWindow";
            hasCoreWindow = true;
        }
        for (auto& child : el.children) findCoreWindow(child);
    };
    findCoreWindow(root);

    // Only attempt injection if there are actually CoreWindow elements
    if (hasCoreWindow) {
        inject_and_collect_xaml_tree(root, hwnd, pid, L"", L"Windows.UI.Xaml.dll", "xaml");
    }
}

} // namespace lvt
