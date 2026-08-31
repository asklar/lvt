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

bool WpfProvider::enrich(Element& root, HWND hwnd, DWORD pid) {
    label_wpf_windows(root);
    auto connection = open_connection(hwnd, pid);
    return connection && connection->get_tree(root, false);
}

std::shared_ptr<IFrameworkConnection> WpfProvider::open_connection(HWND hwnd, DWORD pid) {
    return open_wpf_connection(hwnd, pid);
}

bool WpfProvider::enrich_with_connection(
    Element& root, IFrameworkConnection& connection) {
    label_wpf_windows(root);
    return connection.get_tree(root, false);
}

} // namespace lvt
