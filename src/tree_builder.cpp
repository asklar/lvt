#include "tree_builder.h"
#include "lvt_config.h"
#include "element_key.h"
#include "providers/provider.h"
#include "providers/win32_provider.h"
#include "providers/comctl_provider.h"
#if LVT_ENABLE_XAML
#include "providers/xaml_provider.h"
#endif
#if LVT_ENABLE_WINUI3
#include "providers/winui3_provider.h"
#endif
#if LVT_ENABLE_WPF
#include "providers/wpf_provider.h"
#endif
#if LVT_ENABLE_WINFORMS
#include "providers/winforms_provider.h"
#endif
#include "plugin_loader.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>

namespace lvt {

static void assign_ids_recursive(Element& el, int& counter) {
    el.id = "e" + std::to_string(counter++);
    for (auto& child : el.children) {
        assign_ids_recursive(child, counter);
    }
}

static void trim_to_depth_impl(Element& el, int currentDepth, int maxDepth) {
    if (maxDepth >= 0 && currentDepth >= maxDepth) {
        el.children.clear();
        return;
    }
    for (auto& child : el.children) {
        trim_to_depth_impl(child, currentDepth + 1, maxDepth);
    }
}

void assign_element_ids(Element& root) {
    int counter = 0;
    assign_ids_recursive(root, counter);
}

Element* find_element_by_id(Element& root, const std::string& id) {
    if (root.id == id) return &root;
    for (auto& child : root.children) {
        auto* found = find_element_by_id(child, id);
        if (found) return found;
    }
    return nullptr;
}

const Element* find_element_by_id(const Element& root, const std::string& id) {
    if (root.id == id) return &root;
    for (auto& child : root.children) {
        auto* found = find_element_by_id(child, id);
        if (found) return found;
    }
    return nullptr;
}

Element* find_element_by_key(Element& root, const std::string& key) {
    if (root.key == key) return &root;
    for (auto& child : root.children) {
        auto* found = find_element_by_key(child, key);
        if (found) return found;
    }
    return nullptr;
}

const Element* find_element_by_key(const Element& root, const std::string& key) {
    if (root.key == key) return &root;
    for (auto& child : root.children) {
        auto* found = find_element_by_key(child, key);
        if (found) return found;
    }
    return nullptr;
}

Element* find_element_by_ref(Element& root, const std::string& ref) {
    if (auto* byId = find_element_by_id(root, ref))
        return byId;
    return find_element_by_key(root, ref);
}

const Element* find_element_by_ref(const Element& root, const std::string& ref) {
    if (auto* byId = find_element_by_id(root, ref))
        return byId;
    return find_element_by_key(root, ref);
}

std::optional<std::string> get_element_property(const Element& element, const std::string& property) {
    if (property == "id") return element.id;
    if (property == "key") return element.key;
    if (property == "type") return element.type;
    if (property == "framework") return element.framework;
    if (property == "className") return element.className;
    if (property == "text") return element.text;
    if (property == "bounds") {
        std::ostringstream out;
        out << element.bounds.x << "," << element.bounds.y << ","
            << element.bounds.width << "," << element.bounds.height;
        return out.str();
    }

    auto it = element.properties.find(property);
    if (it != element.properties.end()) return it->second;
    return std::nullopt;
}

void trim_to_depth(Element& root, int maxDepth) {
    trim_to_depth_impl(root, 0, maxDepth);
}

Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks,
                   int maxDepth, const std::string& pluginOption) {
    // Start with the Win32 provider as the base — it always applies
    Win32Provider win32;
    Element root = win32.build(hwnd, maxDepth);

    // Layer on framework-specific providers
    for (auto& fi : frameworks) {
        switch (fi.type) {
        case Framework::ComCtl: {
            ComCtlProvider comctl;
            comctl.enrich(root);
            break;
        }
        case Framework::Xaml: {
#if LVT_ENABLE_XAML
            XamlProvider xaml;
            xaml.enrich(root, hwnd, pid);
#endif
            break;
        }
        case Framework::WinUI3: {
#if LVT_ENABLE_WINUI3
            WinUI3Provider winui3;
            winui3.enrich(root, hwnd, pid);
#endif
            break;
        }
        case Framework::Wpf: {
#if LVT_ENABLE_WPF
            WpfProvider wpf;
            wpf.enrich(root, hwnd, pid);
#endif
            break;
        }
        case Framework::WinForms: {
#if LVT_ENABLE_WINFORMS
            WinFormsProvider winforms;
            winforms.enrich(root, hwnd, pid);
#endif
            break;
        }
        case Framework::Plugin: {
            // Look up the plugin by name and enrich
            for (auto& p : get_plugins()) {
                if (p.info && p.info->name && fi.name == p.info->name) {
                    PluginFrameworkInfo pf;
                    pf.name = fi.name;
                    pf.version = fi.version;
                    pf.plugin = &p;
                    enrich_with_plugin(root, hwnd, pid, pf, pluginOption);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // Assign IDs on the full tree so that element IDs are stable regardless of --depth.
    assign_element_ids(root);
    assign_element_keys(root);

    return root;
}

} // namespace lvt
