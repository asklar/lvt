#include "xaml_provider.h"
#include "xaml_diag_common.h"
#include <cstdio>
#include <functional>
#include <Windows.h>

namespace lvt {

// Shared by enrich(), open_connection() and enrich_with_connection() so all
// three resolve "which element is the CoreWindow" identically - a second,
// drifted implementation is exactly the kind of divergence this codebase
// has been bitten by before (see element_key.cpp's history).
static Element* find_core_window(Element& root) {
    Element* coreWindow = nullptr;
    std::function<void(Element&)> findCoreWindow = [&](Element& el) {
        if (el.className == "Windows.UI.Core.CoreWindow") {
            el.framework = "xaml";
            el.type = "CoreWindow";
            if (!coreWindow) coreWindow = &el;
        }
        for (auto& child : el.children) findCoreWindow(child);
    };
    findCoreWindow(root);
    return coreWindow;
}

void XamlProvider::enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties) {
    Element* coreWindow = find_core_window(root);
    if (!coreWindow) return;

    // UWP apps: the CoreWindow belongs to the actual app process (e.g. CalculatorApp.exe),
    // not the ApplicationFrameHost.exe that owns the top-level window.
    // We must inject into the CoreWindow's owning process.
    DWORD corePid = pid;
    if (coreWindow->nativeHandle) {
        HWND coreHwnd = reinterpret_cast<HWND>(coreWindow->nativeHandle);
        GetWindowThreadProcessId(coreHwnd, &corePid);
    }

    inject_and_collect_xaml_tree(*coreWindow, hwnd, corePid, L"", L"Windows.UI.Xaml.dll", "xaml",
                               L"VisualDiagConnection", fastProperties);
}

std::shared_ptr<IFrameworkConnection> XamlProvider::open_connection(const Element& root, HWND hwnd, DWORD pid) {
    // find_core_window mutates framework/type labels as a side effect (see
    // its comment) - harmless here since `root` is only used to resolve
    // corePid, but taking a non-const local copy of the pointer requires
    // casting away const rather than duplicating the walk. Simpler: just
    // walk read-only for the one thing this needs.
    const Element* coreWindow = nullptr;
    std::function<void(const Element&)> find = [&](const Element& el) {
        if (!coreWindow && el.className == "Windows.UI.Core.CoreWindow") coreWindow = &el;
        for (auto& child : el.children) find(child);
    };
    find(root);
    if (!coreWindow) return nullptr;

    DWORD corePid = pid;
    if (coreWindow->nativeHandle) {
        HWND coreHwnd = reinterpret_cast<HWND>(coreWindow->nativeHandle);
        GetWindowThreadProcessId(coreHwnd, &corePid);
    }

    return make_xaml_diag_connection(hwnd, corePid, L"", L"Windows.UI.Xaml.dll", "xaml",
                                     L"VisualDiagConnection");
}

void XamlProvider::enrich_with_connection(Element& root, IFrameworkConnection& connection, bool fastProperties) {
    Element* coreWindow = find_core_window(root);
    if (!coreWindow) return;
    connection.get_tree(*coreWindow, fastProperties);
}

} // namespace lvt
