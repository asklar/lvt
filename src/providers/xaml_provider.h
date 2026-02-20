#pragma once
#include "provider.h"

namespace lvt {

class XamlProvider : public IProvider {
public:
    // Enrich the element tree with UWP XAML visual tree information.
    // Injects lvt_tap.dll into the target process via InitializeXamlDiagnosticsEx
    // and reads the XAML visual tree over a named pipe.
    void enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
