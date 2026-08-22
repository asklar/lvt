#pragma once
#include "provider.h"

namespace lvt {

class WinUI3Provider : public IProvider {
public:
    // Enrich the element tree with WinUI 3 visual tree information.
    // Injects lvt_tap.dll via InitializeXamlDiagnosticsEx targeting
    // Microsoft.UI.Xaml.dll in the target process.
    // `fastProperties` — see xaml_diag_common.h's inject_and_collect_xaml_tree.
    void enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties = false);
};

} // namespace lvt
