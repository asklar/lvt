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
    j["id"] = el.id;
    j["type"] = sanitize(el.type);
    j["framework"] = el.framework;
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

// --- XML serialization ---

static std::string xml_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  r += "&amp;";  break;
        case '<':  r += "&lt;";   break;
        case '>':  r += "&gt;";   break;
        case '"':  r += "&quot;"; break;
        case '\'': r += "&apos;"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20)
                r += c;
        }
    }
    return r;
}

// Make a valid XML tag name from a type string
static std::string xml_tag(const std::string& type) {
    std::string tag;
    for (char c : type) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != '<' && c != '>' && c != ' ')
            tag += c;
    }
    if (tag.empty() || !(isalpha((unsigned char)tag[0]) || tag[0] == '_'))
        tag = "Element";
    return tag;
}

static void element_to_xml(const Element& el, std::ostringstream& out, int indent) {
    std::string pad(indent * 2, ' ');
    std::string tag = xml_tag(el.type);

    out << pad << "<" << tag;
    out << " id=\"" << xml_escape(el.id) << "\"";
    out << " framework=\"" << xml_escape(el.framework) << "\"";
    if (!el.className.empty() && el.className != el.type)
        out << " className=\"" << xml_escape(el.className) << "\"";
    if (!el.text.empty())
        out << " text=\"" << xml_escape(el.text) << "\"";
    if (el.bounds.width > 0 || el.bounds.height > 0)
        out << " bounds=\"" << el.bounds.x << "," << el.bounds.y
            << "," << el.bounds.width << "," << el.bounds.height << "\"";

    for (auto& [k, v] : el.properties) {
        out << " " << xml_escape(k) << "=\"" << xml_escape(v) << "\"";
    }

    if (el.children.empty()) {
        out << " />\n";
    } else {
        out << ">\n";
        for (auto& child : el.children) {
            element_to_xml(child, out, indent + 1);
        }
        out << pad << "</" << tag << ">\n";
    }
}

std::string serialize_to_xml(const Element& root, HWND hwnd, DWORD pid,
                             const std::string& processName,
                             const std::vector<std::string>& frameworks) {
    std::ostringstream out;

    std::ostringstream hwndStr;
    hwndStr << "0x" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8)
            << reinterpret_cast<uintptr_t>(hwnd);

    out << "<LiveVisualTree";
    out << " hwnd=\"" << hwndStr.str() << "\"";
    out << " pid=\"" << pid << "\"";
    out << " process=\"" << xml_escape(processName) << "\"";
    out << " frameworks=\"";
    for (size_t i = 0; i < frameworks.size(); i++) {
        if (i) out << ",";
        out << xml_escape(frameworks[i]);
    }
    out << "\">\n";

    element_to_xml(root, out, 1);

    out << "</LiveVisualTree>\n";
    return out.str();
}

} // namespace lvt
