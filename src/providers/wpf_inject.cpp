#include "wpf_inject.h"

#include "../bounds_util.h"
#include "managed_connection.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iomanip>
#include <sstream>

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

uint64_t json_handle(
    const json& node, const char* name,
    int defaultBase = 10) {
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

void graft_json_node(const json& node, Element& parent, const std::string& framework) {
    Element element;
    element.framework = framework;
    element.className = sanitize(node.value("type", ""));
    const auto lastDot = element.className.rfind('.');
    element.type =
        lastDot == std::string::npos ? element.className : element.className.substr(lastDot + 1);

    element.providerHandle = json_handle(node, "managedHandle");
    if (element.providerHandle != 0) {
        element.properties["managedHandle"] = hex_handle(element.providerHandle);
        element.properties["handleKind"] = "managed";
    }
    const uint64_t hwnd =
        json_handle(node, "hwnd", 16);
    if (hwnd != 0) {
        element.nativeHandle = static_cast<uintptr_t>(hwnd);
        element.properties["hwnd"] = hex_handle(hwnd);
    }

    const std::string name = sanitize(node.value("name", ""));
    if (!name.empty())
        element.properties["name"] = name;

    if (node.contains("text") && node["text"].is_string())
        element.text = sanitize(node["text"].get<std::string>());
    else
        element.text = name;

    const double width = node.value("width", 0.0);
    const double height = node.value("height", 0.0);
    const double offsetX = node.value("offsetX", 0.0);
    const double offsetY = node.value("offsetY", 0.0);
    if (width > 0 && height > 0) {
        const auto x = safe_double_to_int(offsetX);
        const auto y = safe_double_to_int(offsetY);
        const auto convertedWidth = safe_double_to_int(width);
        const auto convertedHeight = safe_double_to_int(height);
        if (x && y && convertedWidth && convertedHeight) {
            element.bounds.x = *x;
            element.bounds.y = *y;
            element.bounds.width = *convertedWidth;
            element.bounds.height = *convertedHeight;
        }
    } else if (node.value("zeroSize", false)) {
        element.properties["zeroSize"] = "true";
    }

    if (node.contains("visible") && node["visible"].is_boolean() &&
        !node["visible"].get<bool>()) {
        element.properties["visible"] = "false";
    }
    if (node.contains("wpf.visibility") && node["wpf.visibility"].is_string())
        element.properties["wpf.visibility"] = node["wpf.visibility"].get<std::string>();
    if (node.contains("enabled") && node["enabled"].is_boolean() &&
        !node["enabled"].get<bool>()) {
        element.properties["enabled"] = "false";
    }

    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"])
            graft_json_node(child, element, framework);
    }
    parent.children.push_back(std::move(element));
}

bool apply_wpf_tree_json(Element& root, const std::string& jsonText) {
    auto roots = wpf_parse_tree_json(jsonText, "wpf");
    if (!roots)
        return false;
    for (auto& element : *roots)
        root.children.push_back(std::move(element));
    return true;
}

} // namespace

std::optional<std::vector<Element>> wpf_parse_tree_json(
    const std::string& jsonText, const std::string& framework) {
    const json tree = json::parse(jsonText, nullptr, false);
    if (tree.is_discarded()) {
        fprintf(stderr, "lvt: failed to parse WPF tree JSON\n");
        return std::nullopt;
    }

    Element syntheticParent;
    if (tree.is_array()) {
        for (const auto& node : tree)
            graft_json_node(node, syntheticParent, framework);
    } else if (tree.is_object()) {
        graft_json_node(tree, syntheticParent, framework);
    }
    return std::move(syntheticParent.children);
}

std::shared_ptr<IFrameworkConnection> open_wpf_connection(HWND hwnd, DWORD pid) {
    ManagedConnectionOptions options;
    options.frameworkLabel = "wpf";
    options.tapStem = L"lvt_wpf_tap";
    options.managedAssemblyName = L"LvtWpfTap.dll";
    options.pipePrefix = L"lvt_wpf_";
    options.sidecarStem = L"lvt_wpf_pipe";
    return open_managed_framework_connection(
        hwnd, pid, std::move(options), apply_wpf_tree_json);
}

bool inject_and_collect_wpf_tree(Element& root, HWND hwnd, DWORD pid) {
    auto connection = open_wpf_connection(hwnd, pid);
    return connection && connection->get_tree(root, false);
}

} // namespace lvt
