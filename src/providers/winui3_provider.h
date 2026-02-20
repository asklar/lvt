#pragma once
#include "provider.h"

namespace lvt {

class WinUI3Provider : public IProvider {
public:
    // Enrich the element tree with WinUI 3 visual tree information.
    // Injects lvt_tap.dll via InitializeXamlDiagnosticsEx targeting
    // Microsoft.UI.Xaml.dll in the target process.
    void enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
