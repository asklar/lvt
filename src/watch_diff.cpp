#include "watch_diff.h"
#include "element_key.h"
#include <nlohmann/json.hpp>
#include <map>
#include <sstream>

namespace lvt {

using json = nlohmann::json;

namespace {

// Flattens an already-keyed tree (see index_tree below) into depth-first,
// path-ordered records for diffing. Deliberately does NOT compute identity
// itself — `element->key` is trusted to already be correct, because
// index_tree always calls assign_element_keys first. Keeping this file from
// ever inventing its own, second key algorithm is the whole point: it is
// exactly what went wrong before (see element_key.h's assign_element_keys
// doc comment).
struct IndexedElement {
    const Element* element = nullptr;
    std::string path;
};

void collect_index(const Element& el, const std::string& path, std::vector<IndexedElement>& out) {
    out.push_back({&el, path});
    for (size_t i = 0; i < el.children.size(); ++i) {
        auto childPath = path.empty() ? std::to_string(i) : path + "." + std::to_string(i);
        collect_index(el.children[i], childPath, out);
    }
}

// Assigns stable keys (assign_element_keys — the same, single algorithm
// dump/query/UIA output use) and flattens the now-keyed tree into
// path-ordered records. `root` is mutated (its elements' `.key` fields are
// (re)computed) — safe and idempotent even if a caller already assigned
// keys upstream (build_tree/build_uia_tree both do, for instance): calling
// it again here is what makes this file self-sufficient rather than
// depending on every caller remembering to key their tree beforehand,
// which is exactly the assumption that silently broke last time.
std::vector<IndexedElement> index_tree(Element& root) {
    assign_element_keys(root);
    std::vector<IndexedElement> elements;
    collect_index(root, "0", elements);
    return elements;
}

} // namespace

static std::string bounds_to_string(const Bounds& b) {
    return std::to_string(b.x) + "," + std::to_string(b.y) + "," +
           std::to_string(b.width) + "," + std::to_string(b.height);
}

static json bounds_to_json(const Bounds& b) {
    return json{{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}};
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
    j["key"] = el.key;
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

// Same fields as element_to_json, but never recurses into children. A watch
// event ("added"/"removed") describes exactly one node; the client rebuilds
// tree structure from the flat stream of per-node events and each event's
// "path" field (see LiveTree.RebuildHierarchy in the viewer), never from
// nested children on an individual event. Element::children on the copy
// snapshot_added_events/diff_trees attaches to each ChangeEvent already
// holds that node's full subtree (Element is a value type; a copy is deep),
// so calling the recursive serializer here embedded every descendant's data
// again inside every one of its ancestors' own "added" events — for a tree
// with N nodes D levels deep, up to O(N*D) redundant data instead of O(N).
// Measured against a real ~1400-node, ~20-level-deep WinUI3 tree (Microsoft
// Store), that redundancy produced hundreds of megabytes of stdout for a
// single tick and looked, from the client's side reading that flood, just
// like a connection that would never finish.
static json element_to_json_flat(const Element& el) {
    json j;
    j["id"] = el.id;
    j["key"] = el.key;
    j["type"] = el.type;
    j["framework"] = el.framework;
    if (!el.className.empty()) j["className"] = el.className;
    if (!el.text.empty()) j["text"] = el.text;
    j["bounds"] = bounds_to_json(el.bounds);
    if (!el.properties.empty())
        j["properties"] = el.properties;
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

// A ChangeEvent describes exactly one node; nothing reads its element's
// children (see element_to_json_flat's comment). Element's copy constructor
// deep-copies `children` (a std::vector<Element>) recursively, so
// `event.element = *indexed.element` copies that node's *entire subtree*
// just to immediately discard it at serialization time — for a tree N nodes
// deep and D levels deep, that is wasted work up to O(N*D), not O(N), for
// every event this file produces. Building the flat copy field-by-field
// instead of via the copy constructor avoids ever touching `children` here.
static Element element_without_children(const Element& el) {
    Element flat;
    flat.id = el.id;
    flat.key = el.key;
    flat.type = el.type;
    flat.framework = el.framework;
    flat.className = el.className;
    flat.text = el.text;
    flat.bounds = el.bounds;
    flat.properties = el.properties;
    flat.nativeHandle = el.nativeHandle;
    return flat;
}

std::vector<ChangeEvent> snapshot_added_events(Element& root) {
    std::vector<ChangeEvent> events;
    for (const auto& indexed : index_tree(root)) {
        ChangeEvent event;
        event.type = ChangeEvent::Type::Added;
        event.key = indexed.element->key;
        event.path = indexed.path;
        event.element = element_without_children(*indexed.element);
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<ChangeEvent> diff_trees(Element& before, Element& after) {
    // Each side is keyed independently — assign_element_keys' local, per-
    // parent scoping needs nothing from the other tree to produce a stable
    // key, unlike the old global-count algorithm this replaced, which had
    // to look at both sides together specifically to decide consistently
    // whether a disambiguating suffix was needed at all.
    std::vector<IndexedElement> beforeElements = index_tree(before);
    std::vector<IndexedElement> afterElements = index_tree(after);

    std::map<std::string, IndexedElement> beforeByKey;
    std::map<std::string, IndexedElement> afterByKey;
    for (const auto& indexed : beforeElements)
        beforeByKey[indexed.element->key] = indexed;
    for (const auto& indexed : afterElements)
        afterByKey[indexed.element->key] = indexed;

    std::vector<ChangeEvent> events;

    for (const auto& [key, indexed] : afterByKey) {
        if (beforeByKey.find(key) != beforeByKey.end())
            continue;
        ChangeEvent event;
        event.type = ChangeEvent::Type::Added;
        event.key = key;
        event.path = indexed.path;
        event.element = element_without_children(*indexed.element);
        events.push_back(std::move(event));
    }

    for (const auto& [key, indexed] : beforeByKey) {
        if (afterByKey.find(key) != afterByKey.end())
            continue;
        ChangeEvent event;
        event.type = ChangeEvent::Type::Removed;
        event.key = key;
        event.path = indexed.path;
        event.element = element_without_children(*indexed.element);
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
        event.element = element_without_children(*afterIt->second.element);
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
        j["element"] = element_to_json_flat(event.element);
    }

    return j.dump();
}

} // namespace lvt
