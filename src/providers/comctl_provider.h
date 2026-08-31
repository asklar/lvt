#pragma once
#include "provider.h"
#include "../target.h"

namespace lvt {

class NativePropertyConnection;

class ComCtlProvider : public IProvider {
public:
    // Enrich an existing Win32 element tree with ComCtl-specific details.
    // Walks the tree and for any HWND whose class matches a known ComCtl class,
    // replaces/augments the element with richer information.
    void enrich(
        Element& root, Architecture targetArchitecture,
        NativePropertyConnection* properties = nullptr);

private:
    void enrich_recursive(
        Element& el, Architecture targetArchitecture,
        NativePropertyConnection* properties);
    void enrich_listview(
        Element& el, HWND hwnd, bool pointerAllowed,
        NativePropertyConnection* properties);
    void enrich_treeview(
        Element& el, HWND hwnd, bool pointerAllowed,
        NativePropertyConnection* properties);
    void enrich_toolbar(
        Element& el, HWND hwnd, bool pointerAllowed,
        NativePropertyConnection* properties);
    void enrich_statusbar(
        Element& el, HWND hwnd, bool pointerAllowed,
        NativePropertyConnection* properties);
    void enrich_tabcontrol(
        Element& el, HWND hwnd, bool pointerAllowed,
        NativePropertyConnection* properties);
};

} // namespace lvt
