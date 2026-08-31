#include "watch_diff.h"
#include "element_key.h"
#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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
    // A scoped watch tree already carries the key assigned while it was still
    // attached to the complete tree. Preserve that root identity; recomputing
    // it in isolation would discard the ancestor-qualified durable key that
    // the caller used to select the scope.
    assign_element_keys(root, true);
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

struct ProcessWideProviderIdentity {
    std::string framework;
    uint64_t handle = 0;

    bool operator<(const ProcessWideProviderIdentity& other) const {
        return framework < other.framework ||
               (framework == other.framework && handle < other.handle);
    }
};

template <typename ElementPtr>
struct GlobalCandidate {
    ElementPtr element = nullptr;
    size_t count = 0;
};

struct GlobalReconciliation {
    std::unordered_map<const Element*, Element*> prevToCurr;
    std::unordered_map<const Element*, const Element*> currToPrev;
    std::unordered_map<const Element*, std::string> prevPaths;
    std::unordered_map<const Element*, std::string> currPaths;
    std::unordered_map<const Element*, const Element*> prevParents;
    std::unordered_map<const Element*, const Element*> currParents;
    std::unordered_set<const Element*> processedCurr;
};

std::string authoritative_identity_key(const Element& el) {
    return base_identity_key(el) + '\0' + el.durableIdentity;
}

void collect_authoritative_identity_counts(
    const Element& el, std::map<std::string, size_t>& counts) {
    if (!el.durableIdentity.empty())
        ++counts[authoritative_identity_key(el)];
    for (const auto& child : el.children)
        collect_authoritative_identity_counts(child, counts);
}

void collect_prev_candidates(
    const Element& el, const Element* parent, const std::string& path,
    std::map<ProcessWideProviderIdentity, GlobalCandidate<const Element*>>& candidates,
    GlobalReconciliation& result) {
    result.prevParents.emplace(&el, parent);
    result.prevPaths.emplace(&el, path);
    if (has_process_wide_provider_identity(el)) {
        auto& candidate = candidates[{el.framework, el.providerHandle}];
        candidate.element = &el;
        candidate.count++;
    }
    for (size_t i = 0; i < el.children.size(); ++i)
        collect_prev_candidates(el.children[i], &el,
                                path + "." + std::to_string(i),
                                candidates, result);
}

void collect_curr_candidates(
    Element& el, Element* parent, const std::string& path,
    std::map<ProcessWideProviderIdentity, GlobalCandidate<Element*>>& candidates,
    GlobalReconciliation& result) {
    result.currParents.emplace(&el, parent);
    result.currPaths.emplace(&el, path);
    if (has_process_wide_provider_identity(el)) {
        auto& candidate = candidates[{el.framework, el.providerHandle}];
        candidate.element = &el;
        candidate.count++;
    }
    for (size_t i = 0; i < el.children.size(); ++i)
        collect_curr_candidates(el.children[i], &el,
                                path + "." + std::to_string(i),
                                candidates, result);
}

GlobalReconciliation build_global_reconciliation(Element& before, Element& after) {
    std::map<ProcessWideProviderIdentity, GlobalCandidate<const Element*>> prevCandidates;
    std::map<ProcessWideProviderIdentity, GlobalCandidate<Element*>> currCandidates;
    GlobalReconciliation result;
    collect_prev_candidates(before, nullptr, "0", prevCandidates, result);
    collect_curr_candidates(after, nullptr, "0", currCandidates, result);
    std::map<std::string, size_t> prevDurableCounts;
    std::map<std::string, size_t> currDurableCounts;
    collect_authoritative_identity_counts(before, prevDurableCounts);
    collect_authoritative_identity_counts(after, currDurableCounts);

    for (const auto& [identity, prevCandidate] : prevCandidates) {
        auto currIt = currCandidates.find(identity);
        if (currIt == currCandidates.end() ||
            prevCandidate.count != 1 || currIt->second.count != 1)
            continue;

        auto* prev = prevCandidate.element;
        auto* curr = currIt->second.element;
        if (base_identity_key(*prev) != base_identity_key(*curr))
            continue;
        if (!prev->durableIdentity.empty() ||
            !curr->durableIdentity.empty()) {
            if (prev->durableIdentity.empty() ||
                curr->durableIdentity.empty() ||
                prev->durableIdentity != curr->durableIdentity) {
                continue;
            }
            const auto durableKey =
                authoritative_identity_key(*prev);
            if (prevDurableCounts[durableKey] != 1 ||
                currDurableCounts[durableKey] != 1) {
                continue;
            }
        }

        result.prevToCurr.emplace(prev, curr);
        result.currToPrev.emplace(curr, prev);
    }
    return result;
}

// Matches `prevParent`'s children against `currParent`'s children.
// `matched` receives (prevIndex, currIndex) pairs; `removedOut`/`addedOut`
// receive the indices of whatever could not be matched on either side.
void reconcile_children(const Element& prevParent, Element& currParent,
                        GlobalReconciliation& global,
                        std::vector<std::pair<size_t, size_t>>& matched,
                        std::vector<std::pair<const Element*, Element*>>& movedIn,
                        std::vector<size_t>& removedOut,
                        std::vector<size_t>& addedOut) {
    const auto& prevChildren = prevParent.children;
    auto& currChildren = currParent.children;

    std::unordered_map<uint64_t, size_t> prevByProviderHandle;
    std::unordered_map<uint64_t, size_t> prevByNativeLifetime;
    std::unordered_map<uintptr_t, size_t> prevByNativeHandle;
    std::unordered_map<std::string, size_t> prevByName;
    struct DurableCandidate {
        size_t index = 0;
        size_t count = 0;
    };
    std::map<std::string, DurableCandidate> prevByDurableIdentity;
    std::map<std::string, DurableCandidate> currByDurableIdentity;
    for (size_t i = 0; i < prevChildren.size(); i++) {
        const auto& p = prevChildren[i];
        if (global.prevToCurr.find(&p) != global.prevToCurr.end())
            continue;
        if (!p.durableIdentity.empty()) {
            auto& candidate =
                prevByDurableIdentity[authoritative_identity_key(p)];
            candidate.index = i;
            ++candidate.count;
            continue;
        }
        if (has_durable_provider_identity(p))
            prevByProviderHandle.emplace(p.providerHandle, i);
        if (p.nativeLifetimeHandle != 0)
            prevByNativeLifetime.emplace(
                p.nativeLifetimeHandle, i);
        else if (p.nativeHandle != 0)
            prevByNativeHandle.emplace(p.nativeHandle, i);
        auto name = stable_name_key(p);
        if (!name.empty())
            prevByName.emplace(base_identity_key(p) + "|" + name, i);
    }
    for (size_t i = 0; i < currChildren.size(); ++i) {
        const auto& c = currChildren[i];
        if (global.currToPrev.find(&c) != global.currToPrev.end() ||
            c.durableIdentity.empty()) {
            continue;
        }
        auto& candidate =
            currByDurableIdentity[authoritative_identity_key(c)];
        candidate.index = i;
        ++candidate.count;
    }

    std::vector<bool> prevUsed(prevChildren.size(), false);
    std::vector<bool> currUsed(currChildren.size(), false);

    // Process-wide provider identities are matched before any parent-local
    // heuristic. If both endpoints are still direct children of this matched
    // parent pair, treat them like an ordinary sibling match. Otherwise claim
    // the current endpoint as a moved-in subtree root; its old endpoint is
    // suppressed from the old parent's removal list below.
    std::unordered_map<const Element*, size_t> directPrevIndices;
    for (size_t i = 0; i < prevChildren.size(); ++i)
        directPrevIndices.emplace(&prevChildren[i], i);
    // Walk current children in encounter order. Process an opted-in global
    // identity first, then the existing parent-local provider handle, native
    // handle, and stable-name heuristics. The base-identity equality check
    // guards against a recycled handle naming an unrelated element.
    for (size_t ci = 0; ci < currChildren.size(); ci++) {
        const auto& c = currChildren[ci];
        auto globalIt = global.currToPrev.find(&c);
        if (globalIt != global.currToPrev.end()) {
            auto directIt = directPrevIndices.find(globalIt->second);
            if (directIt != directPrevIndices.end()) {
                matched.push_back({directIt->second, ci});
                prevUsed[directIt->second] = true;
            } else {
                movedIn.push_back({globalIt->second, &currChildren[ci]});
            }
            currUsed[ci] = true;
            continue;
        }
        if (!c.durableIdentity.empty()) {
            const auto key = authoritative_identity_key(c);
            const auto currIdentity = currByDurableIdentity.find(key);
            const auto prevIdentity = prevByDurableIdentity.find(key);
            if (currIdentity != currByDurableIdentity.end() &&
                prevIdentity != prevByDurableIdentity.end() &&
                currIdentity->second.count == 1 &&
                prevIdentity->second.count == 1 &&
                !prevUsed[prevIdentity->second.index]) {
                matched.push_back(
                    {prevIdentity->second.index, ci});
                prevUsed[prevIdentity->second.index] = true;
                currUsed[ci] = true;
            }
            // A provider-supplied durable identity is authoritative. If it
            // changed, disappeared, or is ambiguous on either side, do not
            // let provider/native/name/shape fallback preserve the old key.
            continue;
        }
        std::optional<size_t> matchIdx;
        if (has_durable_provider_identity(c)) {
            auto it = prevByProviderHandle.find(c.providerHandle);
            if (it != prevByProviderHandle.end() && !prevUsed[it->second] &&
                base_identity_key(prevChildren[it->second]) == base_identity_key(c))
                matchIdx = it->second;
        }
        if (!matchIdx && c.nativeLifetimeHandle != 0) {
            auto it = prevByNativeLifetime.find(
                c.nativeLifetimeHandle);
            if (it != prevByNativeLifetime.end() &&
                !prevUsed[it->second] &&
                base_identity_key(prevChildren[it->second]) ==
                    base_identity_key(c)) {
                matchIdx = it->second;
            }
        }
        if (!matchIdx && c.nativeLifetimeHandle != 0)
            continue;
        if (!matchIdx && c.nativeLifetimeHandle == 0 &&
            c.nativeHandle != 0) {
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
        if (!prevUsed[i] &&
            prevChildren[i].durableIdentity.empty() &&
            global.prevToCurr.find(&prevChildren[i]) == global.prevToCurr.end())
            prevGroups[base_identity_key(prevChildren[i])].push_back(i);
    for (size_t i = 0; i < currChildren.size(); i++)
        if (!currUsed[i] &&
            currChildren[i].durableIdentity.empty() &&
            global.currToPrev.find(&currChildren[i]) == global.currToPrev.end())
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
    // The grouped matches above marked only their current indices.
    for (auto& [pi, ci] : matched)
        prevUsed[pi] = true;

    for (size_t i = 0; i < prevChildren.size(); i++)
        if (!prevUsed[i] &&
            global.prevToCurr.find(&prevChildren[i]) == global.prevToCurr.end())
            removedOut.push_back(i);
    for (size_t i = 0; i < currChildren.size(); i++)
        if (!currUsed[i] &&
            global.currToPrev.find(&currChildren[i]) == global.currToPrev.end())
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
    flat.durableIdentity = el.durableIdentity;
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

void reconcile_and_collect(const Element& prev, Element& curr,
                           const std::string& prevPath, const std::string& currPath,
                           bool parentChanged,
                           GlobalReconciliation& global,
                           std::vector<ChangeEvent>& events);

// Emits Added events depth-first, except that a globally recognized node is
// reconciled against its previous location instead. This matters when a live
// XAML object is reparented beneath a parent that is itself genuinely new.
void collect_added_events(Element& node, const std::string& path,
                          GlobalReconciliation& global,
                          std::vector<ChangeEvent>& events) {
    auto globalIt = global.currToPrev.find(&node);
    if (globalIt != global.currToPrev.end()) {
        if (global.processedCurr.insert(&node).second) {
            reconcile_and_collect(*globalIt->second, node,
                                  global.prevPaths.at(globalIt->second), path,
                                  /*parentChanged=*/true,
                                  global, events);
        }
        return;
    }

    ChangeEvent event;
    event.type = ChangeEvent::Type::Added;
    event.key = node.key;
    event.path = path;
    event.element = element_without_children(node);
    events.push_back(std::move(event));
    for (size_t i = 0; i < node.children.size(); i++)
        collect_added_events(node.children[i], path + "." + std::to_string(i),
                             global, events);
}

// Emits Removed events depth-first. A globally matched node (and its subtree)
// is suppressed here because its current endpoint owns the one recursive
// reconciliation of that conceptual element.
void collect_removed_events(const Element& node, const std::string& path,
                            const GlobalReconciliation& global,
                            std::vector<ChangeEvent>& events) {
    if (global.prevToCurr.find(&node) != global.prevToCurr.end())
        return;

    ChangeEvent event;
    event.type = ChangeEvent::Type::Removed;
    event.key = node.key;
    event.path = path;
    event.element = element_without_children(node);
    events.push_back(std::move(event));
    for (size_t i = 0; i < node.children.size(); i++)
        collect_removed_events(node.children[i], path + "." + std::to_string(i),
                               global, events);
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
                           bool parentChanged,
                           GlobalReconciliation& global,
                           std::vector<ChangeEvent>& events) {
    global.processedCurr.insert(&curr);
    curr.key = prev.key;

    auto fields = diff_element_fields(prev, prevPath, curr, currPath);
    if (parentChanged) {
        const auto* prevParent = global.prevParents.at(&prev);
        const auto* currParent = global.currParents.at(&curr);
        fields["parentKey"] = {
            prevParent == nullptr ? std::string() : prevParent->key,
            currParent == nullptr ? std::string() : currParent->key,
        };
    }
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
    std::vector<std::pair<const Element*, Element*>> movedIn;
    std::vector<size_t> removed, added;
    reconcile_children(prev, curr, global, matched, movedIn, removed, added);

    for (auto& [pi, ci] : matched)
        reconcile_and_collect(prev.children[pi], curr.children[ci],
                              prevPath + "." + std::to_string(pi),
                              currPath + "." + std::to_string(ci),
                              /*parentChanged=*/false, global, events);
    for (auto& [movedPrev, movedCurr] : movedIn) {
        if (!global.processedCurr.insert(movedCurr).second)
            continue;
        reconcile_and_collect(*movedPrev, *movedCurr,
                              global.prevPaths.at(movedPrev),
                              global.currPaths.at(movedCurr),
                              /*parentChanged=*/true, global, events);
    }
    for (auto ci : added)
        collect_added_events(curr.children[ci], currPath + "." + std::to_string(ci),
                             global, events);
    for (auto pi : removed)
        collect_removed_events(prev.children[pi], prevPath + "." + std::to_string(pi),
                               global, events);
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
    assign_element_keys(before, true);
    assign_element_keys(after, true);

    if ((!before.durableIdentity.empty() ||
         !after.durableIdentity.empty()) &&
        before.durableIdentity != after.durableIdentity) {
        GlobalReconciliation none;
        std::vector<ChangeEvent> replaced;
        collect_removed_events(before, "0", none, replaced);
        collect_added_events(after, "0", none, replaced);
        return replaced;
    }

    auto global = build_global_reconciliation(before, after);
    global.processedCurr.insert(&after);

    std::vector<ChangeEvent> events;
    reconcile_and_collect(before, after, "0", "0",
                          /*parentChanged=*/false, global, events);
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
