#include "winforms_inject.h"

#include "managed_connection.h"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

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

uint64_t parse_handle(const json& node, const char* name, int defaultBase = 10) {
    auto value = node.find(name);
    if (value == node.end())
        return 0;
    if (value->is_number_unsigned())
        return value->get<uint64_t>();
    if (value->is_number_integer()) {
        const auto signedValue = value->get<int64_t>();
        return signedValue > 0 ? static_cast<uint64_t>(signedValue) : 0;
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
        return std::stoull(text.substr(start), nullptr, base);
    } catch (...) {
        return 0;
    }
}

std::string hex_handle(uint64_t handle) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << handle;
    return stream.str();
}

std::string simple_type_name(const std::string& fullType) {
    const auto lastDot = fullType.rfind('.');
    return lastDot == std::string::npos ? fullType : fullType.substr(lastDot + 1);
}

using ElementPath = std::vector<size_t>;

Element* resolve_path(Element& root, const ElementPath& path) {
    Element* current = &root;
    for (size_t childIndex : path) {
        if (childIndex >= current->children.size())
            return nullptr;
        current = &current->children[childIndex];
    }
    return current;
}

void index_by_hwnd(
    Element& element, ElementPath& path,
    std::unordered_map<uintptr_t, ElementPath>& index) {
    if (element.nativeHandle != 0)
        index[element.nativeHandle] = path;
    auto hwnd = element.properties.find("hwnd");
    if (hwnd != element.properties.end()) {
        json value = hwnd->second;
        json wrapper = {{"hwnd", value}};
        const uintptr_t parsed = static_cast<uintptr_t>(
            parse_handle(wrapper, "hwnd", 16));
        if (parsed != 0)
            index[parsed] = path;
    }
    for (size_t childIndex = 0; childIndex < element.children.size(); ++childIndex) {
        path.push_back(childIndex);
        index_by_hwnd(element.children[childIndex], path, index);
        path.pop_back();
    }
}

void apply_properties(Element& element, const json& node) {
    const std::string fullType = sanitize(node.value("type", ""));
    const std::string name = sanitize(node.value("name", ""));
    const std::string text = sanitize(node.value("text", ""));
    const uint64_t hwnd = parse_handle(node, "hwnd", 16);
    const uint64_t managedHandle = parse_handle(node, "managedHandle");

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
        element.providerHandle = managedHandle;
    }
    if (element.nativeHandle == 0 && hwnd != 0)
        element.nativeHandle = static_cast<uintptr_t>(hwnd);

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
    const json& node, std::unordered_map<uintptr_t, ElementPath>& hwndIndex,
    Element& root, const ElementPath& managedParentPath) {
    if (!node.is_object())
        return false;

    const uintptr_t hwnd = static_cast<uintptr_t>(
        parse_handle(node, "hwnd", 16));
    ElementPath elementPath;
    bool foundElement = false;
    if (hwnd != 0) {
        auto existing = hwndIndex.find(hwnd);
        if (existing != hwndIndex.end()) {
            elementPath = existing->second;
            foundElement = true;
        }
    }

    if (!foundElement) {
        Element* managedParent = resolve_path(root, managedParentPath);
        if (!managedParent)
            return false;
        const size_t childIndex = managedParent->children.size();
        managedParent->children.emplace_back();
        elementPath = managedParentPath;
        elementPath.push_back(childIndex);
        foundElement = true;
        if (hwnd != 0)
            hwndIndex[hwnd] = elementPath;
    }

    Element* element = resolve_path(root, elementPath);
    if (!element)
        return false;

    apply_properties(*element, node);
    bool applied = true;
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"])
            applied = apply_control_node(
                          child, hwndIndex, root, elementPath) ||
                      applied;
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

    std::unordered_map<uintptr_t, ElementPath> hwndIndex;
    ElementPath rootPath;
    index_by_hwnd(root, rootPath, hwndIndex);

    bool applied = false;
    if (tree.is_array()) {
        for (const auto& node : tree)
            applied = apply_control_node(
                          node, hwndIndex, root, rootPath) ||
                      applied;
    } else if (tree.is_object()) {
        applied = apply_control_node(tree, hwndIndex, root, rootPath);
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
