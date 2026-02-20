#pragma once
#include "provider.h"

namespace lvt {

class XamlProvider : public IProvider {
public:
    // Enrich the element tree with UWP XAML visual tree information.
    // Connects to the target process's XAML diagnostics via IVisualTreeService3.
    void enrich(Element& root, HWND hwnd);
};

} // namespace lvt
