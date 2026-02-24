#pragma once

// lvt plugin interface — C ABI for runtime-loaded framework provider plugins.
// Plugins are DLLs placed in %USERPROFILE%/.lvt/plugins/ and discovered at startup.
// This header is the ONLY dependency between lvt core and any plugin.

#include <stdint.h>
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LVT_PLUGIN_API_VERSION 1

// ---------- Plugin metadata ----------

struct LvtPluginInfo {
    uint32_t struct_size;       // sizeof(LvtPluginInfo), for versioning
    uint32_t api_version;       // must be LVT_PLUGIN_API_VERSION
    const char* name;           // short identifier, e.g. "myframework"
    const char* description;    // human-readable, e.g. "Custom framework support"
};

// ---------- Framework detection ----------

struct LvtFrameworkDetection {
    uint32_t struct_size;
    const char* name;           // framework name reported by plugin
    const char* version;        // version string or NULL
};

// ---------- Element data (C ABI mirror of lvt::Element) ----------

struct LvtBounds {
    int32_t x, y, width, height;
};

struct LvtProperty {
    const char* key;
    const char* value;
};

struct LvtElementData {
    uint32_t struct_size;
    const char* type;
    const char* framework;
    const char* class_name;
    const char* text;
    LvtBounds bounds;
    const LvtProperty* properties;
    uint32_t property_count;
    struct LvtElementData* children;
    uint32_t child_count;
    uintptr_t native_handle;    // e.g. HWND
};

// ---------- Plugin entry points ----------
// Plugins must export these functions by name.

// Returns static plugin metadata. Called once at load time.
typedef LvtPluginInfo* (*LvtPluginInfoFn)(void);

// Detect if this plugin's framework is present in the target process.
// Returns nonzero if detected, fills `out` with framework info.
// `out` is caller-allocated. Plugin should set name and version fields.
typedef int (*LvtDetectFrameworkFn)(DWORD pid, HWND hwnd, LvtFrameworkDetection* out);

// Enrich the element tree with this plugin's framework data.
// `json_out` receives a malloc'd JSON string (caller frees with lvt_plugin_free).
// The JSON follows the same schema as the XAML TAP DLL output:
//   [{"type":"...", "name":"...", "children":[...], "width":..., "height":..., "offsetX":..., "offsetY":...}]
// `hwnd_filter` is the HWND of a specific host window to scope enrichment to,
// or NULL for all.
// Returns nonzero on success.
typedef int (*LvtEnrichTreeFn)(HWND hwnd, DWORD pid, const char* element_class_filter, char** json_out);

// Free memory allocated by the plugin (e.g. json_out from LvtEnrichTreeFn).
typedef void (*LvtPluginFreeFn)(void* ptr);

// Exported function names (for GetProcAddress)
#define LVT_PLUGIN_INFO_FUNC      "lvt_plugin_info"
#define LVT_PLUGIN_DETECT_FUNC    "lvt_detect_framework"
#define LVT_PLUGIN_ENRICH_FUNC    "lvt_enrich_tree"
#define LVT_PLUGIN_FREE_FUNC      "lvt_plugin_free"

#ifdef __cplusplus
}
#endif
