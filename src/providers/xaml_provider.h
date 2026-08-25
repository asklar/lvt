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
    // `root` (this tick's Win32 base tree) to resolve which process actually
    // owns the XAML content — the same resolution enrich() does internally,
    // exposed here so a caller only pays for it once instead of every
    // refresh. Returns nullptr if no CoreWindow is present in `root`, or the
    // connection could not be established.
    std::shared_ptr<IFrameworkConnection> open_connection(const Element& root, HWND hwnd, DWORD pid);

    // Re-locates the CoreWindow in the CURRENT tick's `root` (a fresh Win32
    // walk happens every tick, so an Element pointer from a previous tick is
    // never valid) and refreshes it over the already-open `connection`
    // instead of re-injecting. No-op if `root` has no CoreWindow this tick.
    void enrich_with_connection(Element& root, IFrameworkConnection& connection, bool fastProperties = false);
};

} // namespace lvt
