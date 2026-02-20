#include "tree_builder.h"
#include "providers/provider.h"
#include "providers/win32_provider.h"
#include "providers/comctl_provider.h"
#include "providers/xaml_provider.h"
#include "providers/winui3_provider.h"
#include <algorithm>
#include <memory>

namespace lvt {

static void assign_ids_recursive(Element& el, int& counter) {
    el.id = "e" + std::to_string(counter++);
    for (auto& child : el.children) {
        assign_ids_recursive(child, counter);
    }
}

void assign_element_ids(Element& root) {
    int counter = 0;
    assign_ids_recursive(root, counter);
}

Element build_tree(HWND hwnd, const std::vector<Framework>& frameworks, int maxDepth) {
    // Start with the Win32 provider as the base — it always applies
    Win32Provider win32;
    Element root = win32.build(hwnd, maxDepth);

    // Layer on framework-specific providers
    for (auto f : frameworks) {
        switch (f) {
        case Framework::ComCtl: {
            ComCtlProvider comctl;
            comctl.enrich(root);
            break;
        }
        case Framework::Xaml: {
            XamlProvider xaml;
            xaml.enrich(root, hwnd);
            break;
        }
        case Framework::WinUI3: {
            WinUI3Provider winui3;
            winui3.enrich(root, hwnd);
            break;
        }
        default:
            break;
        }
    }

    assign_element_ids(root);
    return root;
}

} // namespace lvt
