#include "winforms_inject.h"

#include "managed_connection.h"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace lvt {

using json = nlohmann::json;

namespace {

std::string sanitize(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        if (static_cast<unsigned char>(character) >= 0x20 || character == '\t')
            result += character;
    }
    return result;
}

uintptr_t parse_handle(const json& node, const char* name, int defaultBase = 10) {
    auto value = node.find(name);
    if (value == node.end())
        return 0;
    if (value->is_number_unsigned())
        return static_cast<uintptr_t>(value->get<uint64_t>());
    if (value->is_number_integer()) {
        const auto signedValue = value->get<int64_t>();
        return signedValue > 0 ? static_cast<uintptr_t>(signedValue) : 0;
    }
    if (!value->is_string())
        return 0;

    std::string text = value->get<std::string>();
    int base = defaultBase;
    size_t start = 0;
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        base = 16;
        start = 2;
    }
    try {
        return static_cast<uintptr_t>(std::stoull(text.substr(start), nullptr, base));
    } catch (...) {
        return 0;
    }
}

std::string hex_handle(uintptr_t handle) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << handle;
    return stream.str();
}

std::string simple_type_name(const std::string& fullType) {
    const auto lastDot = fullType.rfind('.');
    return lastDot == std::string::npos ? fullType : fullType.substr(lastDot + 1);
}

void index_by_hwnd(Element& element, std::unordered_map<uintptr_t, Element*>& index) {
    if (element.nativeHandle != 0)
        index[element.nativeHandle] = &element;
    auto hwnd = element.properties.find("hwnd");
    if (hwnd != element.properties.end()) {
        json value = hwnd->second;
        json wrapper = {{"hwnd", value}};
        const uintptr_t parsed = parse_handle(wrapper, "hwnd", 16);
        if (parsed != 0)
            index[parsed] = &element;
    }
    for (auto& child : element.children)
        index_by_hwnd(child, index);
}

void apply_properties(Element& element, const json& node) {
    const std::string fullType = sanitize(node.value("type", ""));
    const std::string name = sanitize(node.value("name", ""));
    const std::string text = sanitize(node.value("text", ""));
    const uintptr_t hwnd = parse_handle(node, "hwnd", 16);
    const uintptr_t managedHandle = parse_handle(node, "managedHandle");

    element.framework = "winforms";
    if (!fullType.empty()) {
        element.className = fullType;
        element.properties["winforms.type"] = fullType;
        element.type = simple_type_name(fullType);
    }
    if (!name.empty()) {
        element.properties["name"] = name;
        element.properties["winforms.name"] = name;
    }
    if (!text.empty())
        element.properties["winforms.text"] = text;
    if (hwnd != 0)
        element.properties["hwnd"] = hex_handle(hwnd);
    if (managedHandle != 0) {
        element.properties["managedHandle"] = hex_handle(managedHandle);
        element.properties["handleKind"] = hwnd != 0 ? "hwnd+managed" : "managed";
    }
    if (element.nativeHandle == 0)
        element.nativeHandle = hwnd != 0 ? hwnd : managedHandle;

    if (node.contains("visible") && node["visible"].is_boolean())
        element.properties["winforms.visible"] =
            node["visible"].get<bool>() ? "true" : "false";
    if (node.contains("enabled") && node["enabled"].is_boolean())
        element.properties["winforms.enabled"] =
            node["enabled"].get<bool>() ? "true" : "false";
    if (node.contains("readOnly") && node["readOnly"].is_boolean())
        element.properties["readOnly"] =
            node["readOnly"].get<bool>() ? "true" : "false";
    if (node.contains("autoSize") && node["autoSize"].is_boolean())
        element.properties["autoSize"] =
            node["autoSize"].get<bool>() ? "true" : "false";
}

bool apply_control_node(
    const json& node, std::unordered_map<uintptr_t, Element*>& hwndIndex,
    Element* managedParent) {
    if (!node.is_object())
        return false;

    const uintptr_t hwnd = parse_handle(node, "hwnd", 16);
    Element* element = nullptr;
    if (hwnd != 0) {
        auto existing = hwndIndex.find(hwnd);
        if (existing != hwndIndex.end())
            element = existing->second;
    }

    if (!element && managedParent) {
        managedParent->children.emplace_back();
        element = &managedParent->children.back();
        if (hwnd != 0)
            hwndIndex[hwnd] = element;
    }
    if (!element)
        return false;

    apply_properties(*element, node);
    bool applied = true;
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"])
            applied = apply_control_node(child, hwndIndex, element) || applied;
    }
    return applied;
}

} // namespace

bool apply_winforms_control_json(Element& root, const std::string& jsonText) {
    if (jsonText.empty())
        return false;
    const json tree = json::parse(jsonText, nullptr, false);
    if (tree.is_discarded())
        return false;

    std::unordered_map<uintptr_t, Element*> hwndIndex;
    index_by_hwnd(root, hwndIndex);

    bool applied = false;
    if (tree.is_array()) {
        for (const auto& node : tree)
            applied = apply_control_node(node, hwndIndex, &root) || applied;
    } else if (tree.is_object()) {
        applied = apply_control_node(tree, hwndIndex, &root);
    }
    return applied;
}

std::shared_ptr<IFrameworkConnection> open_winforms_connection(HWND hwnd, DWORD pid) {
    ManagedConnectionOptions options;
    options.frameworkLabel = "winforms";
    options.tapStem = L"lvt_winforms_tap";
    options.managedAssemblyName = L"LvtWinFormsTap.dll";
    options.pipePrefix = L"lvt_winforms_";
    options.sidecarStem = L"lvt_winforms_pipe";
    return open_managed_framework_connection(
        hwnd, pid, std::move(options), apply_winforms_control_json);
}

bool inject_and_collect_winforms_tree(Element& root, HWND hwnd, DWORD pid) {
    auto connection = open_winforms_connection(hwnd, pid);
    return connection && connection->get_tree(root, false);
}

} // namespace lvt
