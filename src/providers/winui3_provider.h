#pragma once
#include "provider.h"

namespace lvt {

class WinUI3Provider : public IProvider {
public:
    // Enrich the element tree with WinUI 3 visual tree information.
    // Connects to Microsoft.UI.Xaml diagnostics in the target process.
    void enrich(Element& root, HWND hwnd);
};

} // namespace lvt
