#pragma once

// lvt plugin interface — C ABI for runtime-loaded framework provider plugins.
// Plugins are DLLs placed in %USERPROFILE%/.lvt/plugins/ and discovered at startup.
// This header is the ONLY dependency between lvt core and any plugin.

#include <stdint.h>
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped from 1 to 2 to add the OPTIONAL persistent-connection functions
// below (see "Persistent connections"). This is additive, not a breaking
// change: lvt_loader.cpp accepts any api_version from 1 up to this value
// (not just an exact match), and every v2 function is probed individually
// via GetProcAddress — a v1-only plugin that has never been rebuilt simply
// doesn't export them, and lvt core falls back to the same one-shot
// lvt_enrich_tree path it always used. A plugin only needs to bump its own
// reported api_version once it actually implements the v2 functions.
#define LVT_PLUGIN_API_VERSION 2

// ---------- Plugin metadata ----------

struct LvtPluginInfo {
    uint32_t struct_size;       // sizeof(LvtPluginInfo), for versioning
    uint32_t api_version;       // the highest LVT_PLUGIN_API_VERSION this plugin actually implements
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
//
// This one-shot path always works and is what every v1 plugin implements.
// A plugin that also implements the v2 functions below still needs this one:
// it is the fallback lvt core uses whenever no persistent connection is
// available (a lvt_connection_open failure, or a caller — a one-shot CLI
// command — that never asked the registry for one at all).
typedef int (*LvtEnrichTreeFn)(HWND hwnd, DWORD pid, const char* element_class_filter, char** json_out);

// Free memory allocated by the plugin (e.g. json_out from LvtEnrichTreeFn).
typedef void (*LvtPluginFreeFn)(void* ptr);

// ---------- Persistent connections (optional, API v2) ----------
//
// Mirrors src/providers/framework_connection.h's IFrameworkConnection: a
// plugin that implements these lets lvt core (see connection_registry.h)
// establish a connection ONCE and reuse it for many tree refreshes across a
// watch session or an MCP session, instead of calling lvt_enrich_tree fresh
// every single time - the same problem this whole mechanism exists to avoid
// for XAML/WinUI3 (see docs/tap-dll-design.md's connection lifecycle).
//
// Every function here is independently optional: lvt core probes each by
// name via GetProcAddress and only uses the group at all if
// lvt_connection_open is present. A plugin that implements none of them
// keeps working exactly as it does today via lvt_enrich_tree.

struct LvtConnectionEvent {
    uint32_t struct_size;
    const char* mutation;       // "add" | "remove"
    uintptr_t handle;
    uintptr_t parent_handle;
    int32_t child_index;
    const char* element_type;   // only meaningful for "add"
    const char* name;           // only meaningful for "add"
};

// Establishes a persistent connection to (hwnd, pid). Returns an opaque,
// plugin-owned handle, or NULL if unsupported or the connection could not
// be established. lvt core treats NULL exactly like a v1 plugin that has no
// lvt_connection_open at all: it falls back to lvt_enrich_tree.
typedef void* (*LvtConnectionOpenFn)(HWND hwnd, DWORD pid);

// Re-collects the current tree over `conn` (no re-injection) and yields it
// the same way lvt_enrich_tree does: a malloc'd JSON string in `json_out`,
// freed by the caller via lvt_plugin_free.
typedef int (*LvtConnectionGetTreeFn)(void* conn, const char* element_class_filter, char** json_out);

// Non-blocking: fills `events_out`/`count_out` with whatever incremental
// Add/Remove notifications the plugin has observed since the last call.
// Returns nonzero on success (including "success, zero events"); a plugin
// that never implements real incremental tracking can simply always report
// zero events here - callers always have lvt_connection_get_tree as a full
// refresh fallback, so this is never the only way to get current data.
typedef int (*LvtConnectionPollEventsFn)(void* conn, LvtConnectionEvent** events_out, uint32_t* count_out);

// Frees an array returned by lvt_connection_poll_events.
typedef void (*LvtConnectionEventsFreeFn)(LvtConnectionEvent* events, uint32_t count);

// Closes a connection opened by lvt_connection_open - the plugin's chance
// to do whatever clean teardown its underlying mechanism needs, exactly
// once, when the connection actually ends (not per refresh).
typedef void (*LvtConnectionCloseFn)(void* conn);

// Exported function names (for GetProcAddress)
#define LVT_PLUGIN_INFO_FUNC      "lvt_plugin_info"
#define LVT_PLUGIN_DETECT_FUNC    "lvt_detect_framework"
#define LVT_PLUGIN_ENRICH_FUNC    "lvt_enrich_tree"
#define LVT_PLUGIN_FREE_FUNC      "lvt_plugin_free"

// v2, all optional - see "Persistent connections" above.
#define LVT_PLUGIN_CONNECTION_OPEN_FUNC         "lvt_connection_open"
#define LVT_PLUGIN_CONNECTION_GET_TREE_FUNC     "lvt_connection_get_tree"
#define LVT_PLUGIN_CONNECTION_POLL_EVENTS_FUNC  "lvt_connection_poll_events"
#define LVT_PLUGIN_CONNECTION_EVENTS_FREE_FUNC  "lvt_connection_events_free"
#define LVT_PLUGIN_CONNECTION_CLOSE_FUNC        "lvt_connection_close"

#ifdef __cplusplus
}
#endif

