#include "json_serializer.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

namespace lvt {

using json = nlohmann::json;

static json bounds_to_json(const Bounds& b) {
    return json{{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}};
}

static json element_to_json(const Element& el) {
    json j;
    j["id"] = el.id;
    j["type"] = el.type;
    j["framework"] = el.framework;
    // Strip control characters from strings (XAML runtime can include them in type names)
    auto sanitize = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (static_cast<unsigned char>(c) >= 0x20)
                r += c;
        }
        return r;
    };
    if (!el.className.empty()) j["className"] = sanitize(el.className);
    if (!el.text.empty()) j["text"] = sanitize(el.text);
    j["bounds"] = bounds_to_json(el.bounds);

    if (!el.properties.empty()) {
        json props = json::object();
        for (auto& [k, v] : el.properties) {
            props[k] = v;
        }
        j["properties"] = props;
    }

    if (!el.children.empty()) {
        json kids = json::array();
        for (auto& child : el.children) {
            kids.push_back(element_to_json(child));
        }
        j["children"] = kids;
    }

    return j;
}

std::string serialize_to_json(const Element& root, HWND hwnd, DWORD pid,
                              const std::string& processName,
                              const std::vector<std::string>& frameworks) {
    json output;

    // Target info
    std::ostringstream hwndStr;
    hwndStr << "0x" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8)
            << reinterpret_cast<uintptr_t>(hwnd);
    output["target"] = json{
        {"hwnd", hwndStr.str()},
        {"pid", pid},
        {"processName", processName}
    };

    output["frameworks"] = frameworks;
    output["root"] = element_to_json(root);

    return output.dump(2);
}

} // namespace lvt
