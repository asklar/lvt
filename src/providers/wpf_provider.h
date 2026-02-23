#pragma once
#include "provider.h"

namespace lvt {

class WpfProvider : public IProvider {
public:
    // Enrich the element tree with WPF visual tree information.
    // Labels HwndWrapper windows and (future) injects managed TAP DLL
    // to walk the WPF visual tree via VisualTreeHelper.
    void enrich(Element& root, HWND hwnd, DWORD pid);
};

} // namespace lvt
