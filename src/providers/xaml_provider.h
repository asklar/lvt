#pragma once
#include "provider.h"
#include "framework_connection.h"
#include <memory>

namespace lvt {

class XamlProvider : public IProvider {
public:
    // Enrich the element tree with UWP XAML visual tree information.
    // Injects lvt_tap.dll into the target process via InitializeXamlDiagnosticsEx
    // and reads the XAML visual tree over a named pipe.
    // `fastProperties` — see xaml_diag_common.h's inject_and_collect_xaml_tree.
    void enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties = false);

    // Establishes a persistent connection (see framework_connection.h) for
    // reuse across many refreshes, e.g. by watch's loop or an MCP session —
    // see connection_registry.h. Uses the CoreWindow already present in
    // `root` (UWP), or a Windows.UI.Composition.DesktopWindowContentBridge
    // (desktop system-XAML island), to resolve which process owns the XAML
    // content. This is the same resolution enrich() does internally, exposed
    // so a caller only pays for it once instead of every refresh. Returns
    // nullptr when neither supported host is present, or connection fails.
    std::shared_ptr<IFrameworkConnection> open_connection(const Element& root, HWND hwnd, DWORD pid);

    // Re-locates the CoreWindow in the CURRENT tick's `root` (a fresh Win32
    // walk happens every tick, so an Element pointer from a previous tick is
    // never valid) and refreshes it over the already-open `connection`
    // instead of re-injecting. No-op if `root` has no CoreWindow this tick.
    void enrich_with_connection(Element& root, IFrameworkConnection& connection, bool fastProperties = false);
};

} // namespace lvt
