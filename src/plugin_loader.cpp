#include "plugin_loader.h"
#include "debug.h"
#include "bounds_util.h"
#include "module_util.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <set>
#include <utility>
#include <userenv.h>
#include <wil/resource.h>

#pragma comment(lib, "userenv.lib")

using json = nlohmann::json;

namespace lvt {

static std::vector<LoadedPlugin> s_plugins;

// Directories searched for plugins, in priority order:
//   1. $LVT_PLUGIN_DIR
//   2. <directory of the lvt binary>/plugins, so a packaged install works
//   3. %USERPROFILE%\.lvt\plugins, for user-installed plugins
static std::vector<std::wstring> get_plugin_dirs() {
    std::vector<std::wstring> dirs;

    std::wstring override(MAX_PATH, L'\0');
    DWORD len = GetEnvironmentVariableW(L"LVT_PLUGIN_DIR", override.data(),
                                        static_cast<DWORD>(override.size()));
    if (len > override.size()) {
        override.resize(len);
        len = GetEnvironmentVariableW(L"LVT_PLUGIN_DIR", override.data(),
                                      static_cast<DWORD>(override.size()));
    }
    if (len > 0) {
        override.resize(len);
        dirs.push_back(override);
    }

    auto moduleDir = get_module_dir();
    if (!moduleDir.empty())
        dirs.push_back(moduleDir + L"\\plugins");

    wchar_t profileDir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profileDir, MAX_PATH))
        dirs.push_back(std::wstring(profileDir) + L"\\.lvt\\plugins");

    return dirs;
}

static void load_plugins_from(const std::wstring& dir, std::set<std::wstring>& seen) {
    std::wstring pattern = dir + L"\\*.dll";
    WIN32_FIND_DATAW fd;
    wil::unique_hfind hFind(FindFirstFileW(pattern.c_str(), &fd));
    if (!hFind) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // A plugin of the same name in an earlier directory wins.
        std::wstring lowerName(fd.cFileName);
        for (auto& c : lowerName) c = towlower(c);
        if (!seen.insert(lowerName).second) continue;

        std::wstring fullPath = dir + L"\\" + fd.cFileName;
        wil::unique_hmodule mod(LoadLibraryW(fullPath.c_str()));
        if (!mod) {
            if (g_debug)
                fprintf(stderr, "lvt: failed to load plugin %ls (error %lu)\n",
                        fd.cFileName, GetLastError());
            continue;
        }

        auto infoFn = reinterpret_cast<LvtPluginInfoFn>(
            GetProcAddress(mod.get(), LVT_PLUGIN_INFO_FUNC));
        if (!infoFn) {
            if (g_debug)
                fprintf(stderr, "lvt: %ls has no %s export, skipping\n",
                        fd.cFileName, LVT_PLUGIN_INFO_FUNC);
            continue;
        }

        LvtPluginInfo* info = infoFn();
        // Accept any version from 1 up to what this core supports, not
        // just an exact match: a plugin reporting api_version=1 (built
        // before the optional v2 connection functions existed - see
        // plugin.h) must keep loading and working via lvt_enrich_tree,
        // exactly as it always did. Only reject something newer than this
        // core understands, or malformed metadata.
        if (!info || info->struct_size < sizeof(LvtPluginInfo) ||
            info->api_version < 1 || info->api_version > LVT_PLUGIN_API_VERSION) {
            if (g_debug)
                fprintf(stderr, "lvt: %ls has incompatible plugin API version\n",
                        fd.cFileName);
            continue;
        }

        LoadedPlugin lp{};
        lp.module = std::move(mod);
        lp.info = info;
        lp.detect = reinterpret_cast<LvtDetectFrameworkFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_DETECT_FUNC));
        lp.enrich = reinterpret_cast<LvtEnrichTreeFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_ENRICH_FUNC));
        lp.free_fn = reinterpret_cast<LvtPluginFreeFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_FREE_FUNC));
        // Every one of these is independently optional - see plugin.h's
        // "Persistent connections". A plugin still on v1 simply doesn't
        // export them, GetProcAddress returns nullptr, and every consumer
        // of LoadedPlugin already treats a null function pointer as "this
        // plugin doesn't support that" (open_plugin_connection below
        // requires connection_open specifically to be non-null before
        // trying to use any of the rest).
        lp.connection_open = reinterpret_cast<LvtConnectionOpenFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_CONNECTION_OPEN_FUNC));
        lp.connection_get_tree = reinterpret_cast<LvtConnectionGetTreeFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_CONNECTION_GET_TREE_FUNC));
        lp.connection_poll_events = reinterpret_cast<LvtConnectionPollEventsFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_CONNECTION_POLL_EVENTS_FUNC));
        lp.connection_events_free = reinterpret_cast<LvtConnectionEventsFreeFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_CONNECTION_EVENTS_FREE_FUNC));
        lp.connection_close = reinterpret_cast<LvtConnectionCloseFn>(
            GetProcAddress(lp.module.get(), LVT_PLUGIN_CONNECTION_CLOSE_FUNC));

        const bool supportsPersistentConnections =
            lp.connection_open && lp.connection_get_tree &&
            lp.connection_close && lp.free_fn;
        if (g_debug)
            fprintf(stderr, "lvt: loaded plugin '%s' (%s)%s\n",
                    info->name ? info->name : "?",
                    info->description ? info->description : "",
                    supportsPersistentConnections ? ", supports persistent connections" : "");

        s_plugins.push_back(std::move(lp));
    } while (FindNextFileW(hFind.get(), &fd));
}

void load_plugins() {
    std::set<std::wstring> seen;
    for (const auto& dir : get_plugin_dirs())
        load_plugins_from(dir, seen);
}

void unload_plugins() {
    s_plugins.clear();
}

const std::vector<LoadedPlugin>& get_plugins() {
    return s_plugins;
}

std::vector<PluginFrameworkInfo> detect_plugin_frameworks(HWND hwnd, DWORD pid) {
    std::vector<PluginFrameworkInfo> result;
    for (auto& p : s_plugins) {
        if (!p.detect) continue;
        LvtFrameworkDetection det{};
        det.struct_size = sizeof(det);
        if (p.detect(pid, hwnd, &det)) {
            PluginFrameworkInfo pfi;
            pfi.name = det.name ? det.name : p.info->name;
            pfi.version = det.version ? det.version : "";
            pfi.plugin = &p;
            result.push_back(std::move(pfi));
            if (g_debug)
                fprintf(stderr, "lvt: plugin '%s' detected framework '%s' %s\n",
                        p.info->name, pfi.name.c_str(), pfi.version.c_str());
        }
    }
    return result;
}

// Strip control characters (same as sanitize in xaml_diag_common.cpp)
static std::string sanitize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t')
            r += c;
    }
    return r;
}

// Recursively graft JSON nodes into an Element tree.
static void graft_json_node(const json& j, Element& parent, const std::string& framework,
                            double parentOffsetX = 0, double parentOffsetY = 0) {
    Element el;
    el.framework = framework;
    el.className = sanitize(j.value("type", ""));
    el.text = sanitize(j.value("text", ""));
    auto name = sanitize(j.value("name", ""));
    if (!name.empty())
        el.properties["name"] = name;
    if (el.text.empty())
        el.text = name;

    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double absX = std::isfinite(ox) ? parentOffsetX + ox : parentOffsetX;
    double absY = std::isfinite(oy) ? parentOffsetY + oy : parentOffsetY;
    if (w > 0 && h > 0) {
        auto sx = safe_double_to_int(absX);
        auto sy = safe_double_to_int(absY);
        auto sw = safe_double_to_int(w);
        auto sh = safe_double_to_int(h);
        if (sx && sy && sw && sh) {
            el.bounds.x = *sx;
            el.bounds.y = *sy;
            el.bounds.width = *sw;
            el.bounds.height = *sh;
        }
    }

    // Copy additional properties if present
    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [key, val] : j["properties"].items()) {
            el.properties[key] = val.is_string() ? val.get<std::string>() : val.dump();
        }
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_json_node(child, el, framework, absX, absY);
        }
    }

    parent.children.push_back(std::move(el));
}

// Grafts an already-parsed plugin tree JSON payload into `root` - shared by
// the one-shot path (enrich_with_plugin) and a reused PluginConnection's
// repeated get_tree() calls, so both share exactly one implementation of
// this logic (mirrors xaml_diag_common.cpp's graft_xaml_tree_json split for
// the same reason).
static void graft_plugin_tree_json(const json& treeJson, Element& root, const std::string& frameworkName) {
    // The plugin JSON is an array of tree roots. Each root has a "target_hwnd"
    // field (hex HWND string) indicating which existing element to graft under.
    // We walk the tree fresh for each root to find the matching host element by
    // its "hwnd" property, avoiding stale pointers from vector reallocations.
    if (treeJson.is_array()) {
        for (auto& node : treeJson) {
            std::string targetHwnd = node.value("target_hwnd", "");

            // Find the element whose "hwnd" property matches target_hwnd
            Element* host = nullptr;
            if (!targetHwnd.empty()) {
                std::function<Element*(Element&)> findHost = [&](Element& el) -> Element* {
                    auto it = el.properties.find("hwnd");
                    if (it != el.properties.end() && it->second == targetHwnd)
                        return &el;
                    for (auto& child : el.children) {
                        auto* found = findHost(child);
                        if (found) return found;
                    }
                    return nullptr;
                };
                host = findHost(root);
            }

            if (host) {
                double baseX = host->bounds.x;
                double baseY = host->bounds.y;
                if (node.contains("children") && node["children"].is_array()) {
                    for (auto& child : node["children"]) {
                        graft_json_node(child, *host, frameworkName, baseX, baseY);
                    }
                } else {
                    graft_json_node(node, *host, frameworkName, baseX, baseY);
                }
            } else {
                // No matching host — graft under root
                graft_json_node(node, root, frameworkName,
                                root.bounds.x, root.bounds.y);
            }
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, frameworkName);
    }
}

bool enrich_with_plugin(Element& root, HWND hwnd, DWORD pid,
                        const PluginFrameworkInfo& pluginFw,
                        const std::string& pluginOption) {
    if (!pluginFw.plugin || !pluginFw.plugin->enrich) return false;

    char* jsonOut = nullptr;
    int ok = pluginFw.plugin->enrich(hwnd, pid,
                                     pluginOption.empty() ? nullptr : pluginOption.c_str(),
                                     &jsonOut);
    if (!ok || !jsonOut) return false;

    json treeJson;
    try {
        treeJson = json::parse(jsonOut);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "lvt: failed to parse plugin JSON: %s\n", e.what());
        if (pluginFw.plugin->free_fn) pluginFw.plugin->free_fn(jsonOut);
        return false;
    }

    if (g_debug)
        fprintf(stderr, "lvt: plugin '%s' returned %zu bytes of tree data\n",
                pluginFw.name.c_str(), strlen(jsonOut));

    if (pluginFw.plugin->free_fn) pluginFw.plugin->free_fn(jsonOut);

    graft_plugin_tree_json(treeJson, root, pluginFw.name);
    return true;
}

// IFrameworkConnection adapter over a plugin's optional v2 connection
// functions (see plugin.h). Lets connection_registry.h's ConnectionRegistry
// treat a plugin-provided connection identically to XamlDiagConnection -
// callers (watch's loop, an MCP session) don't need to know or care which
// one they got.
class PluginConnection : public IFrameworkConnection {
public:
    PluginConnection(const LoadedPlugin* plugin, void* handle, std::string frameworkName)
        : m_plugin(plugin), m_handle(handle), m_frameworkName(std::move(frameworkName)) {
    }

    ~PluginConnection() override {
        if (m_handle && m_plugin->connection_close)
            m_plugin->connection_close(m_handle);
    }

    bool get_tree(Element& root, bool /*fastProperties*/,
                  const std::string& providerOption = {}) override {
        // Plugins have no fast/full distinction today - see plugin.h's
        // LvtConnectionGetTreeFn. Accepting and ignoring the parameter here
        // (rather than omitting it) keeps this a drop-in IFrameworkConnection,
        // consistent with XamlDiagConnection's signature.
        if (!m_handle || !m_plugin->connection_get_tree)
            return false;

        char* jsonOut = nullptr;
        const char* filter = providerOption.empty() ? nullptr : providerOption.c_str();
        int ok = m_plugin->connection_get_tree(m_handle, filter, &jsonOut);
        if (!ok || !jsonOut) {
            m_alive = false;
            return false;
        }

        json treeJson;
        try {
            treeJson = json::parse(jsonOut);
        } catch (const json::parse_error& e) {
            fprintf(stderr, "lvt: failed to parse plugin connection JSON: %s\n", e.what());
            if (m_plugin->free_fn) m_plugin->free_fn(jsonOut);
            return false;
        }
        if (m_plugin->free_fn) m_plugin->free_fn(jsonOut);

        graft_plugin_tree_json(treeJson, root, m_frameworkName);
        return true;
    }

    std::vector<ConnectionEvent> poll_events() override {
        std::vector<ConnectionEvent> result;
        if (!m_handle || !m_plugin->connection_poll_events ||
            !m_plugin->connection_events_free)
            return result;

        LvtConnectionEvent* events = nullptr;
        uint32_t count = 0;
        if (!m_plugin->connection_poll_events(m_handle, &events, &count) || !events)
            return result;

        result.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            ConnectionEvent ev;
            ev.mutation = (events[i].mutation && std::string(events[i].mutation) == "remove")
                              ? ConnectionEvent::Mutation::removed
                              : ConnectionEvent::Mutation::added;
            ev.handle = events[i].handle;
            ev.parentHandle = events[i].parent_handle;
            ev.childIndex = events[i].child_index;
            ev.elementType = events[i].element_type ? events[i].element_type : "";
            ev.name = events[i].name ? events[i].name : "";
            result.push_back(std::move(ev));
        }
        if (m_plugin->connection_events_free)
            m_plugin->connection_events_free(events, count);
        return result;
    }

    bool is_alive() const override { return m_alive && m_handle != nullptr; }

private:
    const LoadedPlugin* m_plugin;
    void* m_handle;
    std::string m_frameworkName;
    bool m_alive = true;
};

std::shared_ptr<IFrameworkConnection> open_plugin_connection(
    const PluginFrameworkInfo& pluginFw, HWND hwnd, DWORD pid) {
    if (!pluginFw.plugin ||
        !pluginFw.plugin->connection_open ||
        !pluginFw.plugin->connection_get_tree ||
        !pluginFw.plugin->connection_close ||
        !pluginFw.plugin->free_fn)
        return nullptr;

    void* handle = pluginFw.plugin->connection_open(hwnd, pid);
    if (!handle)
        return nullptr;

    return std::make_shared<PluginConnection>(pluginFw.plugin, handle, pluginFw.name);
}

} // namespace lvt
