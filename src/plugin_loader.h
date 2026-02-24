#pragma once
#include "plugin.h"
#include "element.h"
#include <string>
#include <vector>
#include <Windows.h>

namespace lvt {

struct LoadedPlugin {
    HMODULE module;
    LvtPluginInfo* info;
    LvtDetectFrameworkFn detect;
    LvtEnrichTreeFn enrich;
    LvtPluginFreeFn free_fn;
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

// Ask all loaded plugins to detect frameworks in the given process.
std::vector<PluginFrameworkInfo> detect_plugin_frameworks(HWND hwnd, DWORD pid);

// Ask the relevant plugin to enrich the tree for a plugin-detected framework.
// Parses the JSON response and grafts elements under matching Win32 nodes.
bool enrich_with_plugin(Element& root, HWND hwnd, DWORD pid,
                        const PluginFrameworkInfo& pluginFw);

} // namespace lvt
