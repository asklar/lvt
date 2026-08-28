#include "winforms_provider.h"
#include "winforms_inject.h"

namespace lvt {

static void label_winforms_like_windows(Element& el) {
    if (el.className.starts_with("WindowsForms10.")) {
        el.framework = "winforms";
        el.properties["winforms.class"] = el.className;
    }
    for (auto& child : el.children) {
        label_winforms_like_windows(child);
    }
}

void WinFormsProvider::enrich(Element& root, HWND hwnd, DWORD pid) {
    label_winforms_like_windows(root);
    auto connection = open_connection(hwnd, pid);
    if (connection)
        connection->get_tree(root, false);
}

std::shared_ptr<IFrameworkConnection> WinFormsProvider::open_connection(
    HWND hwnd, DWORD pid) {
    return open_winforms_connection(hwnd, pid);
}

void WinFormsProvider::enrich_with_connection(
    Element& root, IFrameworkConnection& connection) {
    label_winforms_like_windows(root);
    connection.get_tree(root, false);
}

} // namespace lvt
