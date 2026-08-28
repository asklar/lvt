#pragma once
#include "provider.h"

namespace lvt {

class NativePropertyConnection;

class ComCtlProvider : public IProvider {
public:
    // Enrich an existing Win32 element tree with ComCtl-specific details.
    // Walks the tree and for any HWND whose class matches a known ComCtl class,
    // replaces/augments the element with richer information.
    void enrich(
        Element& root, NativePropertyConnection* properties = nullptr);

private:
    void enrich_recursive(Element& el, NativePropertyConnection* properties);
    void enrich_listview(
        Element& el, HWND hwnd, NativePropertyConnection* properties);
    void enrich_treeview(
        Element& el, HWND hwnd, NativePropertyConnection* properties);
    void enrich_toolbar(
        Element& el, HWND hwnd, NativePropertyConnection* properties);
    void enrich_statusbar(
        Element& el, HWND hwnd, NativePropertyConnection* properties);
    void enrich_tabcontrol(
        Element& el, HWND hwnd, NativePropertyConnection* properties);
};

} // namespace lvt
