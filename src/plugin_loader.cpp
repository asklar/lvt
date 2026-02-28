#include "plugin_loader.h"
#include "debug.h"
#include "bounds_util.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <userenv.h>

#pragma comment(lib, "userenv.lib")

using json = nlohmann::json;

namespace lvt {

static std::vector<LoadedPlugin> s_plugins;

static std::wstring get_plugins_dir() {
    wchar_t profileDir[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!GetEnvironmentVariableW(L"USERPROFILE", profileDir, size))
        return {};
    return std::wstring(profileDir) + L"\\.lvt\\plugins";
}

void load_plugins() {
    auto dir = get_plugins_dir();
    if (dir.empty()) return;

    std::wstring pattern = dir + L"\\*.dll";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring fullPath = dir + L"\\" + fd.cFileName;
        HMODULE mod = LoadLibraryW(fullPath.c_str());
        if (!mod) {
            if (g_debug)
                fprintf(stderr, "lvt: failed to load plugin %ls (error %lu)\n",
                        fd.cFileName, GetLastError());
            continue;
        }

        auto infoFn = reinterpret_cast<LvtPluginInfoFn>(
            GetProcAddress(mod, LVT_PLUGIN_INFO_FUNC));
        if (!infoFn) {
            if (g_debug)
                fprintf(stderr, "lvt: %ls has no %s export, skipping\n",
                        fd.cFileName, LVT_PLUGIN_INFO_FUNC);
            FreeLibrary(mod);
            continue;
        }

        LvtPluginInfo* info = infoFn();
        if (!info || info->struct_size < sizeof(LvtPluginInfo) ||
            info->api_version != LVT_PLUGIN_API_VERSION) {
            if (g_debug)
                fprintf(stderr, "lvt: %ls has incompatible plugin API version\n",
                        fd.cFileName);
            FreeLibrary(mod);
            continue;
        }

        LoadedPlugin lp{};
        lp.module = mod;
        lp.info = info;
        lp.detect = reinterpret_cast<LvtDetectFrameworkFn>(
            GetProcAddress(mod, LVT_PLUGIN_DETECT_FUNC));
        lp.enrich = reinterpret_cast<LvtEnrichTreeFn>(
            GetProcAddress(mod, LVT_PLUGIN_ENRICH_FUNC));
        lp.free_fn = reinterpret_cast<LvtPluginFreeFn>(
            GetProcAddress(mod, LVT_PLUGIN_FREE_FUNC));

        if (g_debug)
            fprintf(stderr, "lvt: loaded plugin '%s' (%s)\n",
                    info->name ? info->name : "?",
                    info->description ? info->description : "");

        s_plugins.push_back(lp);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void unload_plugins() {
    for (auto& p : s_plugins) {
        FreeLibrary(p.module);
    }
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
    if (el.text.empty())
        el.text = sanitize(j.value("name", ""));

    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double absX = std::isfinite(ox) ? parentOffsetX + ox : parentOffsetX;
    double absY = std::isfinite(oy) ? parentOffsetY + oy : parentOffsetY;
    if (w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h)
        && std::isfinite(absX) && std::isfinite(absY)) {
        el.bounds.x = safe_double_to_int(absX);
        el.bounds.y = safe_double_to_int(absY);
        el.bounds.width = safe_double_to_int(w);
        el.bounds.height = safe_double_to_int(h);
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

bool enrich_with_plugin(Element& root, HWND hwnd, DWORD pid,
                        const PluginFrameworkInfo& pluginFw) {
    if (!pluginFw.plugin || !pluginFw.plugin->enrich) return false;

    char* jsonOut = nullptr;
    int ok = pluginFw.plugin->enrich(hwnd, pid, nullptr, &jsonOut);
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
                        graft_json_node(child, *host, pluginFw.name, baseX, baseY);
                    }
                } else {
                    graft_json_node(node, *host, pluginFw.name, baseX, baseY);
                }
            } else {
                // No matching host — graft under root
                graft_json_node(node, root, pluginFw.name,
                                root.bounds.x, root.bounds.y);
            }
        }
    } else if (treeJson.is_object()) {
        graft_json_node(treeJson, root, pluginFw.name);
    }

    return true;
}

} // namespace lvt
