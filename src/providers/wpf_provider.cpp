#include "wpf_provider.h"
#include "wpf_inject.h"
#include <cstdio>
#include <functional>
#include <Windows.h>

namespace lvt {

// Label WPF HwndWrapper windows in the element tree
static void label_wpf_windows(Element& el) {
    if (el.className.starts_with("HwndWrapper[")) {
        el.framework = "wpf";
        el.type = "WpfWindow";
    }
    for (auto& child : el.children) {
        label_wpf_windows(child);
    }
}

void WpfProvider::enrich(Element& root, HWND hwnd, DWORD pid) {
    label_wpf_windows(root);
    inject_and_collect_wpf_tree(root, hwnd, pid);
}

} // namespace lvt
