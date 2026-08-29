#pragma once
#include "plugin.h"
#include "element.h"
#include "providers/framework_connection.h"
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <wil/resource.h>

namespace lvt {

struct LoadedPlugin {
    wil::unique_hmodule module;
    LvtPluginInfo* info;
    LvtDetectFrameworkFn detect;
    LvtEnrichTreeFn enrich;
    LvtPluginFreeFn free_fn;
    // v2 exports (see plugin.h's "Persistent connections"). The core only
    // enables persistence when open/get_tree/close and the v1 free_fn are
    // all present; polling additionally requires its matching free export.
    LvtConnectionOpenFn connection_open = nullptr;
    LvtConnectionGetTreeFn connection_get_tree = nullptr;
    LvtConnectionPollEventsFn connection_poll_events = nullptr;
    LvtConnectionEventsFreeFn connection_events_free = nullptr;
    LvtConnectionCloseFn connection_close = nullptr;
};

// Discover and load plugins from %USERPROFILE%/.lvt/plugins/
void load_plugins();

// Unload all loaded plugins.
void unload_plugins();

// Returns the list of loaded plugins.
const std::vector<LoadedPlugin>& get_plugins();

struct PluginFrameworkInfo {
    std::string name;
    std::string version;
    const LoadedPlugin* plugin;
};

// The persistent tree path is an all-or-nothing lifetime contract. A plugin
// missing any required export remains a one-shot v1 provider.
bool plugin_supports_persistent_connections(const LoadedPlugin& plugin);

// Polling is an independent optional pair. A persistent plugin without it
// still refreshes through connection_get_tree().
bool plugin_supports_event_polling(const LoadedPlugin& plugin);

// Resolve a detected plugin framework back to its loaded plugin.
PluginFrameworkInfo find_plugin_framework(const std::string& name,
                                          const std::string& version = {});

// Registry/lookup key for one plugin connection. Includes the loaded plugin
// instance and target HWND so plugin providers cannot collide with built-in
// labels, with another plugin reporting the same framework name, or with a
// second window in the same process.
std::string plugin_connection_label(const PluginFrameworkInfo& pluginFw,
                                    HWND hwnd);

// Ask all loaded plugins to detect frameworks in the given process.
std::vector<PluginFrameworkInfo> detect_plugin_frameworks(HWND hwnd, DWORD pid);

// Ask the relevant plugin to enrich the tree for a plugin-detected framework.
// Parses the JSON response and grafts elements under matching Win32 nodes.
bool enrich_with_plugin(Element& root, HWND hwnd, DWORD pid,
                        const PluginFrameworkInfo& pluginFw,
                        const std::string& pluginOption = {});

// Establishes a persistent connection (see framework_connection.h) via the
// plugin's optional v2 functions, for reuse across many refreshes the same
// way make_xaml_diag_connection is for XAML/WinUI3 - see
// connection_registry.h for how a caller acquires/reuses/releases one.
// Returns nullptr if the plugin doesn't implement the complete required v2
// lifetime group (open/get_tree/close plus lvt_plugin_free), or connection
// establishment failed.
std::shared_ptr<IFrameworkConnection> open_plugin_connection(
    const PluginFrameworkInfo& pluginFw, HWND hwnd, DWORD pid);

} // namespace lvt
