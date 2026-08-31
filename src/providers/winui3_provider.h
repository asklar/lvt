#pragma once
#include "provider.h"
#include "framework_connection.h"
#include <memory>

namespace lvt {

class WinUI3Provider : public IProvider {
public:
    // Enrich the element tree with WinUI 3 visual tree information.
    // Injects lvt_tap.dll via InitializeXamlDiagnosticsEx targeting
    // Microsoft.UI.Xaml.dll in the target process.
    // `fastProperties` — see xaml_diag_common.h's inject_and_collect_xaml_tree.
    bool enrich(Element& root, HWND hwnd, DWORD pid, bool fastProperties = false);

    // Establishes a persistent connection (see framework_connection.h) for
    // reuse across many refreshes, e.g. by watch's loop or an MCP session —
    // see connection_registry.h. Returns nullptr if the connection could
    // not be established.
    std::shared_ptr<IFrameworkConnection> open_connection(HWND hwnd, DWORD pid);

    // Refreshes `root` over the already-open `connection` instead of
    // re-injecting - the bridge-matching/grafting is unchanged, it just
    // reads from an existing connection rather than a fresh one-shot inject.
    bool enrich_with_connection(Element& root, IFrameworkConnection& connection, bool fastProperties = false);
};

} // namespace lvt
