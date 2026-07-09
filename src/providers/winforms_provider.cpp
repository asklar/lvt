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
    inject_and_collect_winforms_tree(root, hwnd, pid);
}

} // namespace lvt
