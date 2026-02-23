#include "xaml_provider.h"
#include "xaml_diag_common.h"
#include <cstdio>
#include <functional>
#include <Windows.h>

namespace lvt {

void XamlProvider::enrich(Element& root, HWND hwnd, DWORD pid) {
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

    if (!coreWindow) return;

    // UWP apps: the CoreWindow belongs to the actual app process (e.g. CalculatorApp.exe),
    // not the ApplicationFrameHost.exe that owns the top-level window.
    // We must inject into the CoreWindow's owning process.
    DWORD corePid = pid;
    if (coreWindow->nativeHandle) {
        HWND coreHwnd = reinterpret_cast<HWND>(coreWindow->nativeHandle);
        GetWindowThreadProcessId(coreHwnd, &corePid);
    }

    inject_and_collect_xaml_tree(*coreWindow, hwnd, corePid, L"", L"Windows.UI.Xaml.dll", "xaml");
}

} // namespace lvt
