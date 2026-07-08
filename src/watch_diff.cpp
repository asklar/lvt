#include "watch_diff.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>

namespace lvt {

using json = nlohmann::json;

struct IndexedElement {
    const Element* element = nullptr;
    std::string path;
    std::string baseKey;
    std::string key;
};

static std::string bounds_to_string(const Bounds& b) {
    return std::to_string(b.x) + "," + std::to_string(b.y) + "," +
           std::to_string(b.width) + "," + std::to_string(b.height);
}

static json bounds_to_json(const Bounds& b) {
    return json{{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}};
}

static std::string escape_key_part(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '|')
            result += '\\';
        result += c;
    }
    return result;
}

static std::string base_identity_key(const Element& el) {
    return escape_key_part(el.framework) + "|" +
           escape_key_part(el.type) + "|" +
           escape_key_part(el.className);
}

static void collect_index(const Element& el, const std::string& path,
                          std::vector<IndexedElement>& out,
                          std::unordered_map<std::string, int>& counts) {
    IndexedElement indexed;
    indexed.element = &el;
    indexed.path = path;
    indexed.baseKey = base_identity_key(el);
    out.push_back(indexed);
    counts[indexed.baseKey]++;

    for (size_t i = 0; i < el.children.size(); ++i) {
        auto childPath = path.empty() ? std::to_string(i) : path + "." + std::to_string(i);
        collect_index(el.children[i], childPath, out, counts);
    }
}

static void assign_keys(std::vector<IndexedElement>& elements,
                        const std::unordered_map<std::string, int>& counts) {
    for (auto& indexed : elements) {
        indexed.key = indexed.baseKey;
        auto count = counts.find(indexed.baseKey);
        if (count != counts.end() && count->second > 1)
            indexed.key += "|@" + indexed.path;
    }
}

static std::vector<IndexedElement> index_tree(const Element& root) {
    std::vector<IndexedElement> elements;
    std::unordered_map<std::string, int> counts;
    collect_index(root, "0", elements, counts);
    assign_keys(elements, counts);
    return elements;
}

static void index_tree_pair(const Element& before, const Element& after,
                            std::vector<IndexedElement>& beforeElements,
                            std::vector<IndexedElement>& afterElements) {
    std::unordered_map<std::string, int> beforeCounts;
    std::unordered_map<std::string, int> afterCounts;
    collect_index(before, "0", beforeElements, beforeCounts);
    collect_index(after, "0", afterElements, afterCounts);

    std::unordered_map<std::string, int> combinedCounts = beforeCounts;
    for (const auto& [key, count] : afterCounts)
        combinedCounts[key] = std::max(combinedCounts[key], count);

    assign_keys(beforeElements, combinedCounts);
    assign_keys(afterElements, combinedCounts);
}

static std::string property_string(const Element& el, const std::string& name) {
    auto it = el.properties.find(name);
    return it == el.properties.end() ? std::string() : it->second;
}

static void add_field_if_changed(std::map<std::string, FieldChange>& fields,
                                 const std::string& name,
                                 const std::string& before,
                                 const std::string& after) {
    if (before != after)
        fields[name] = {before, after};
}

static std::map<std::string, FieldChange> diff_element_fields(const IndexedElement& before,
                                                              const IndexedElement& after) {
    std::map<std::string, FieldChange> fields;
    add_field_if_changed(fields, "path", before.path, after.path);
    add_field_if_changed(fields, "type", before.element->type, after.element->type);
    add_field_if_changed(fields, "framework", before.element->framework, after.element->framework);
    add_field_if_changed(fields, "className", before.element->className, after.element->className);
    add_field_if_changed(fields, "text", before.element->text, after.element->text);
    add_field_if_changed(fields, "bounds", bounds_to_string(before.element->bounds),
                         bounds_to_string(after.element->bounds));

    std::map<std::string, bool> propertyNames;
    for (const auto& [name, value] : before.element->properties)
        propertyNames[name] = true;
    for (const auto& [name, value] : after.element->properties)
        propertyNames[name] = true;
    for (const auto& [name, ignored] : propertyNames) {
        add_field_if_changed(fields, "properties." + name,
                             property_string(*before.element, name),
                             property_string(*after.element, name));
    }

    return fields;
}

static json element_to_json(const Element& el) {
    json j;
    j["id"] = el.id;
    j["type"] = el.type;
    j["framework"] = el.framework;
    if (!el.className.empty()) j["className"] = el.className;
    if (!el.text.empty()) j["text"] = el.text;
    j["bounds"] = bounds_to_json(el.bounds);
    if (!el.properties.empty())
        j["properties"] = el.properties;
    if (!el.children.empty()) {
        j["children"] = json::array();
        for (const auto& child : el.children)
            j["children"].push_back(element_to_json(child));
    }
    return j;
}

static const char* event_type_name(ChangeEvent::Type type) {
    switch (type) {
    case ChangeEvent::Type::Added: return "added";
    case ChangeEvent::Type::Removed: return "removed";
    case ChangeEvent::Type::Changed: return "changed";
    }
    return "changed";
}

std::vector<ChangeEvent> snapshot_added_events(const Element& root) {
    std::vector<ChangeEvent> events;
    for (const auto& indexed : index_tree(root)) {
        ChangeEvent event;
        event.type = ChangeEvent::Type::Added;
        event.key = indexed.key;
        event.path = indexed.path;
        event.element = *indexed.element;
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<ChangeEvent> diff_trees(const Element& before, const Element& after) {
    std::vector<IndexedElement> beforeElements;
    std::vector<IndexedElement> afterElements;
    index_tree_pair(before, after, beforeElements, afterElements);

    std::map<std::string, IndexedElement> beforeByKey;
    std::map<std::string, IndexedElement> afterByKey;
    for (const auto& indexed : beforeElements)
        beforeByKey[indexed.key] = indexed;
    for (const auto& indexed : afterElements)
        afterByKey[indexed.key] = indexed;

    std::vector<ChangeEvent> events;

    for (const auto& [key, indexed] : afterByKey) {
        if (beforeByKey.find(key) != beforeByKey.end())
            continue;
        ChangeEvent event;
        event.type = ChangeEvent::Type::Added;
        event.key = key;
        event.path = indexed.path;
        event.element = *indexed.element;
        events.push_back(std::move(event));
    }

    for (const auto& [key, indexed] : beforeByKey) {
        if (afterByKey.find(key) != afterByKey.end())
            continue;
        ChangeEvent event;
        event.type = ChangeEvent::Type::Removed;
        event.key = key;
        event.path = indexed.path;
        event.element = *indexed.element;
        events.push_back(std::move(event));
    }

    for (const auto& [key, beforeIndexed] : beforeByKey) {
        auto afterIt = afterByKey.find(key);
        if (afterIt == afterByKey.end())
            continue;
        auto fields = diff_element_fields(beforeIndexed, afterIt->second);
        if (fields.empty())
            continue;
        ChangeEvent event;
        event.type = ChangeEvent::Type::Changed;
        event.key = key;
        event.path = afterIt->second.path;
        event.element = *afterIt->second.element;
        event.fields = std::move(fields);
        events.push_back(std::move(event));
    }

    return events;
}

std::string serialize_change_event(const ChangeEvent& event) {
    json j;
    j["event"] = event_type_name(event.type);
    j["key"] = event.key;
    j["path"] = event.path;

    if (event.type == ChangeEvent::Type::Changed) {
        json fields = json::object();
        for (const auto& [name, change] : event.fields) {
            fields[name] = {{"old", change.oldValue}, {"new", change.newValue}};
        }
        j["fields"] = fields;
    } else {
        j["element"] = element_to_json(event.element);
    }

    return j.dump();
}

} // namespace lvt
