#include "watch_diff.h"
#include "element_key.h"
#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace lvt {

using json = nlohmann::json;

namespace {

// Flattens an already-keyed tree into depth-first, path-ordered records —
// used only for the very first tick (snapshot_added_events), where there is
// no previous tree to reconcile against and everything is simply "added".
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

std::vector<IndexedElement> index_tree(Element& root) {
    assign_element_keys(root);
    std::vector<IndexedElement> elements;
    collect_index(root, "0", elements);
    return elements;
}

// ---- Cross-tick reconciliation ----
//
// See element_key.h's assign_element_keys doc comment for the full story:
// a purely structural key, recomputed fresh from a tree's shape every
// tick with no memory of previous ticks, cannot give a large, live tree a
// stable per-element identity across ticks, because every element's key
// threads through its whole ancestor chain — one unstable ancestor
// anywhere above it changes every single element below it. The functions
// below instead MATCH each tick's tree against the previous one directly,
// node by node, and let a matched node INHERIT its predecessor's key
// outright (see reconcile_and_collect) rather than ever recomputing it.
//
// A one-level-deep "shape" fingerprint (not a full recursive hash) is used
// as a tie-breaker when several same-identity, unnamed siblings exist and
// more than one could plausibly correspond to a given predecessor (e.g. a
// new, empty carousel card inserted ahead of an existing, populated one) —
// preferring the candidate whose own immediate children look like the
// predecessor's over blindly pairing by encounter order, which would
// otherwise attribute the *existing* item's identity to the *new* item
// merely because it now sits at the same position.
std::string shallow_shape_signature(const Element& el) {
    std::string sig;
    for (const auto& child : el.children) {
        if (!sig.empty())
            sig += ",";
        sig += base_identity_key(child);
    }
    return sig;
}

// Matches `prevParent`'s children against `currParent`'s children.
// `matched` receives (prevIndex, currIndex) pairs; `removedOut`/`addedOut`
// receive the indices of whatever could not be matched on either side.
void reconcile_children(const Element& prevParent, const Element& currParent,
                        std::vector<std::pair<size_t, size_t>>& matched,
                        std::vector<size_t>& removedOut,
                        std::vector<size_t>& addedOut) {
    const auto& prevChildren = prevParent.children;
    const auto& currChildren = currParent.children;

    std::unordered_map<uint64_t, size_t> prevByProviderHandle;
    std::unordered_map<uintptr_t, size_t> prevByNativeHandle;
    std::unordered_map<std::string, size_t> prevByName;
    for (size_t i = 0; i < prevChildren.size(); i++) {
        const auto& p = prevChildren[i];
        if (p.providerHandle != 0)
            prevByProviderHandle.emplace(p.providerHandle, i);
        if (p.nativeHandle != 0)
            prevByNativeHandle.emplace(p.nativeHandle, i);
        auto name = stable_name_key(p);
        if (!name.empty())
            prevByName.emplace(base_identity_key(p) + "|" + name, i);
    }

    std::vector<bool> prevUsed(prevChildren.size(), false);
    std::vector<bool> currUsed(currChildren.size(), false);

    // Pass 1+2+3: provider handle, native handle, then a stable name property
    // — all already
    // globally unique-enough on their own (assign_element_keys uses the
    // same two, in the same order, as its own segment discriminators), so
    // matching on either needs no further tie-breaking. The base-identity
    // equality check guards against the pathological case of a hwnd being
    // recycled by Windows for a completely unrelated element.
    for (size_t ci = 0; ci < currChildren.size(); ci++) {
        const auto& c = currChildren[ci];
        std::optional<size_t> matchIdx;
        if (c.providerHandle != 0) {
            auto it = prevByProviderHandle.find(c.providerHandle);
            if (it != prevByProviderHandle.end() && !prevUsed[it->second] &&
                base_identity_key(prevChildren[it->second]) == base_identity_key(c))
                matchIdx = it->second;
        }
        if (!matchIdx && c.nativeHandle != 0) {
            auto it = prevByNativeHandle.find(c.nativeHandle);
            if (it != prevByNativeHandle.end() && !prevUsed[it->second] &&
                base_identity_key(prevChildren[it->second]) == base_identity_key(c))
                matchIdx = it->second;
        }
        if (!matchIdx) {
            auto name = stable_name_key(c);
            if (!name.empty()) {
                auto it = prevByName.find(base_identity_key(c) + "|" + name);
                if (it != prevByName.end() && !prevUsed[it->second])
                    matchIdx = it->second;
            }
        }
        if (matchIdx) {
            matched.push_back({*matchIdx, ci});
            prevUsed[*matchIdx] = true;
            currUsed[ci] = true;
        }
    }

    // Pass 3: whatever is left, grouped by base identity. Within each
    // group, prefer pairing candidates whose own immediate children have
    // the same shape (see shallow_shape_signature) over a blind positional
    // zip, so inserting a new, differently-shaped sibling ahead of an
    // existing one does not hijack the existing one's identity.
    std::map<std::string, std::vector<size_t>> prevGroups, currGroups;
    for (size_t i = 0; i < prevChildren.size(); i++)
        if (!prevUsed[i])
            prevGroups[base_identity_key(prevChildren[i])].push_back(i);
    for (size_t i = 0; i < currChildren.size(); i++)
        if (!currUsed[i])
            currGroups[base_identity_key(currChildren[i])].push_back(i);

    for (auto& [base, plist] : prevGroups) {
        auto it = currGroups.find(base);
        if (it == currGroups.end())
            continue;
        auto& clist = it->second;
        std::vector<bool> clistUsed(clist.size(), false);

        auto tryMatch = [&](size_t pi, bool shapeAware) {
            for (size_t ci = 0; ci < clist.size(); ci++) {
                if (clistUsed[ci])
                    continue;
                if (shapeAware &&
                    shallow_shape_signature(prevChildren[pi]) != shallow_shape_signature(currChildren[clist[ci]]))
                    continue;
                matched.push_back({pi, clist[ci]});
                clistUsed[ci] = true;
                return true;
            }
            return false;
        };

        std::vector<size_t> stillUnmatched;
        for (auto pi : plist)
            if (!tryMatch(pi, /*shapeAware=*/true))
                stillUnmatched.push_back(pi);
        for (auto pi : stillUnmatched)
            tryMatch(pi, /*shapeAware=*/false);

        for (size_t ci = 0; ci < clist.size(); ci++)
            if (clistUsed[ci])
                currUsed[clist[ci]] = true;
    }
    // Mark every matched prev index used (matched above only marked curr).
    for (auto& [pi, ci] : matched)
        prevUsed[pi] = true;

    for (size_t i = 0; i < prevChildren.size(); i++)
        if (!prevUsed[i])
            removedOut.push_back(i);
    for (size_t i = 0; i < currChildren.size(); i++)
        if (!currUsed[i])
            addedOut.push_back(i);
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

static std::map<std::string, FieldChange> diff_element_fields(const Element& before, const std::string& beforePath,
                                                              const Element& after, const std::string& afterPath) {
    std::map<std::string, FieldChange> fields;
    add_field_if_changed(fields, "path", beforePath, afterPath);
    add_field_if_changed(fields, "type", before.type, after.type);
    add_field_if_changed(fields, "framework", before.framework, after.framework);
    add_field_if_changed(fields, "className", before.className, after.className);
    add_field_if_changed(fields, "text", before.text, after.text);
    add_field_if_changed(fields, "bounds", bounds_to_string(before.bounds),
                         bounds_to_string(after.bounds));

    std::map<std::string, bool> propertyNames;
    for (const auto& [name, value] : before.properties)
        propertyNames[name] = true;
    for (const auto& [name, value] : after.properties)
        propertyNames[name] = true;
    for (const auto& [name, ignored] : propertyNames) {
        add_field_if_changed(fields, "properties." + name,
                             property_string(before, name),
                             property_string(after, name));
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
    flat.providerHandle = el.providerHandle;
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

namespace {

// Emits one Added/Removed event per node in `node`'s own subtree (itself
// included), depth-first — matching watch's existing flat, per-node event
// protocol (see watch_diff.h): the client reconstructs structure from the
// stream via each event's own "path", never from nested children on a
// single event.
void collect_subtree_events(const Element& node, const std::string& path,
                            ChangeEvent::Type type, std::vector<ChangeEvent>& events) {
    ChangeEvent event;
    event.type = type;
    event.key = node.key;
    event.path = path;
    event.element = element_without_children(node);
    events.push_back(std::move(event));
    for (size_t i = 0; i < node.children.size(); i++)
        collect_subtree_events(node.children[i], path + "." + std::to_string(i), type, events);
}

// Recursively reconciles `prev` and `curr`, which the caller has already
// established correspond to the same conceptual element (trivially true
// for the roots — a watch session tracks exactly one target window for its
// whole lifetime — and otherwise established by reconcile_children).
// `curr` inherits `prev`'s key outright: see element_key.h's
// assign_element_keys doc comment for why identity must persist this way,
// rather than being recomputed from either tree's structure alone, to
// survive an ancestor's own position among its siblings shifting.
void reconcile_and_collect(const Element& prev, Element& curr,
                          const std::string& prevPath, const std::string& currPath,
                          std::vector<ChangeEvent>& events) {
    curr.key = prev.key;

    auto fields = diff_element_fields(prev, prevPath, curr, currPath);
    if (!fields.empty()) {
        ChangeEvent event;
        event.type = ChangeEvent::Type::Changed;
        event.key = curr.key;
        event.path = currPath;
        event.element = element_without_children(curr);
        event.fields = std::move(fields);
        events.push_back(std::move(event));
    }

    std::vector<std::pair<size_t, size_t>> matched;
    std::vector<size_t> removed, added;
    reconcile_children(prev, curr, matched, removed, added);

    for (auto& [pi, ci] : matched)
        reconcile_and_collect(prev.children[pi], curr.children[ci],
                              prevPath + "." + std::to_string(pi),
                              currPath + "." + std::to_string(ci), events);
    for (auto ci : added)
        collect_subtree_events(curr.children[ci], currPath + "." + std::to_string(ci),
                               ChangeEvent::Type::Added, events);
    for (auto pi : removed)
        collect_subtree_events(prev.children[pi], prevPath + "." + std::to_string(pi),
                               ChangeEvent::Type::Removed, events);
}

} // namespace

std::vector<ChangeEvent> diff_trees(Element& before, Element& after) {
    // Give both trees a fresh, self-describing key as a baseline (the same
    // single algorithm dump/query/UIA output use) — reconcile_and_collect
    // then overwrites `after`'s keys for anything it recognizes as
    // corresponding to something in `before` with `before`'s own
    // (already-inherited-if-applicable) key, so a node's reported identity,
    // once established, survives for as long as it keeps being recognized.
    // Only a node that is genuinely new this tick keeps the key assigned
    // here.
    assign_element_keys(before);
    assign_element_keys(after);

    std::vector<ChangeEvent> events;
    reconcile_and_collect(before, after, "0", "0", events);
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
