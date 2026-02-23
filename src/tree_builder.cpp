#include "tree_builder.h"
#include "providers/provider.h"
#include "providers/win32_provider.h"
#include "providers/comctl_provider.h"
#include "providers/xaml_provider.h"
#include "providers/winui3_provider.h"
#include "providers/wpf_provider.h"
#include <algorithm>
#include <memory>

namespace lvt {

static void assign_ids_recursive(Element& el, int& counter) {
    el.id = "e" + std::to_string(counter++);
    for (auto& child : el.children) {
        assign_ids_recursive(child, counter);
    }
}

static void trim_to_depth_impl(Element& el, int currentDepth, int maxDepth) {
    if (maxDepth >= 0 && currentDepth >= maxDepth) {
        el.children.clear();
        return;
    }
    for (auto& child : el.children) {
        trim_to_depth_impl(child, currentDepth + 1, maxDepth);
    }
}

void assign_element_ids(Element& root) {
    int counter = 0;
    assign_ids_recursive(root, counter);
}

void trim_to_depth(Element& root, int maxDepth) {
    trim_to_depth_impl(root, 0, maxDepth);
}

Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks, int maxDepth) {
    // Start with the Win32 provider as the base — it always applies
    Win32Provider win32;
    Element root = win32.build(hwnd, maxDepth);

    // Layer on framework-specific providers
    for (auto& fi : frameworks) {
        switch (fi.type) {
        case Framework::ComCtl: {
            ComCtlProvider comctl;
            comctl.enrich(root);
            break;
        }
        case Framework::Xaml: {
            XamlProvider xaml;
            xaml.enrich(root, hwnd, pid);
            break;
        }
        case Framework::WinUI3: {
            WinUI3Provider winui3;
            winui3.enrich(root, hwnd, pid);
            break;
        }
        case Framework::Wpf: {
            WpfProvider wpf;
            wpf.enrich(root, hwnd, pid);
            break;
        }
        default:
            break;
        }
    }

    // Assign IDs on the full tree so that element IDs are stable regardless of --depth.
    assign_element_ids(root);

    return root;
}

} // namespace lvt
