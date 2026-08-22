#pragma once
#include "provider.h"

namespace lvt {

class XamlProvider : public IProvider {
public:
    // Enrich the element tree with UWP XAML visual tree information.
    // Injects lvt_tap.dll into the target process via InitializeXamlDiagnosticsEx
    // and reads the XAML visual tree over a named pipe.
    // `fastProperties` — see xaml_diag_common.h's inject_and_collect_xaml_tree.
    void enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties = false);
};

} // namespace lvt
