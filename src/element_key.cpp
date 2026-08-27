#include "element_key.h"
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

namespace lvt {

std::string escape_key_part(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '|' || c == '/' || c == ':')
            result += '\\';
        result += c;
    }
    return result;
}

std::string base_identity_key(const Element& el) {
    // Only one of type/className, not both: for every provider that sets
    // both (xaml_diag_common.cpp, wpf_inject.cpp), `type` is derived as the
    // substring of `className` after its last '.', so it never carries
    // information className does not already have — including both here
    // duplicated it for nothing. className is the more specific of the two
    // when both exist (a raw win32/native class name, or a fully-qualified
    // XAML type), so it wins; type is the fallback for providers where
    // className can be legitimately empty (UIA elements often report no
    // ClassName at all, see uia_provider.cpp), so the key never gets an
    // empty identity segment.
    //
    // This matters far more than it looks: a key is built once per element
    // but then repeated as a "/"-joined prefix in *every one* of that
    // element's descendants (see assign_child_keys below), so halving one
    // segment here roughly halves the key payload of an entire subtree, not
    // just one element. Measured on a real ~1900-element WinUI3 tree
    // (Microsoft Store), the full ancestor-chain "key" made up 40% of the
    // dump's total JSON size before this change.
    const std::string& identity = el.className.empty() ? el.type : el.className;
    return escape_key_part(el.framework) + "|" + escape_key_part(identity);
}

static std::string hwnd_key(uintptr_t handle) {
    std::ostringstream out;
    out << "hwnd:0x" << std::hex << std::uppercase << handle;
    return out.str();
}

// XAML diagnostics InstanceHandles are already process-wide object
// identities. Unlike a sibling index/name path, they do not change when an
// element is reparented and do not need every ancestor repeated in every
// descendant's key. They have also been observed stable across independent
// diagnostics connections to the same live target, which the WinUI
// integration test guards by dumping twice and querying in a third process.
// Keep the structural algorithm as the fallback for providers/elements that
// do not expose such an identity.
static std::string compact_instance_key(const Element& el) {
    if (el.nativeHandle == 0 ||
        (el.framework != "xaml" && el.framework != "winui3"))
        return {};

    std::ostringstream out;
    out << el.framework << ":0x" << std::hex << std::uppercase << el.nativeHandle;
    return out.str();
}

std::string stable_name_key(const Element& el) {
    for (const char* name : {"AutomationId", "x:Name", "Name", "automationId", "name"}) {
        auto it = el.properties.find(name);
        if (it != el.properties.end() && !it->second.empty())
            return std::string(name) + ":" + escape_key_part(it->second);
    }
    return {};
}

// baseCounts/hwndCounts/nameCounts are all scoped to just `parent`'s own
// direct children (see assign_child_keys, which builds them fresh for each
// parent) — never counted across the whole tree. This locality is the
// entire point: a node's key can only ever be disturbed by a change among
// its OWN siblings, never by something elsewhere in an unrelated subtree.
// See assign_element_keys' doc comment for what used to go wrong when a
// different, GLOBALLY-scoped algorithm was used instead (in watch's
// diffing, before this).
static std::string discriminator_for_child(const Element& child, size_t childIndex,
                                           const std::map<std::string, int>& baseCounts,
                                           const std::map<std::string, int>& hwndCounts,
                                           const std::map<std::string, int>& nameCounts) {
    auto base = base_identity_key(child);
    if (child.nativeHandle != 0) {
        auto hwnd = hwnd_key(child.nativeHandle);
        auto full = base + "|" + hwnd;
        auto count = hwndCounts.find(full);
        if (count == hwndCounts.end() || count->second == 1)
            return full;
    }

    auto name = stable_name_key(child);
    if (!name.empty()) {
        auto full = base + "|" + name;
        auto count = nameCounts.find(full);
        if (count == nameCounts.end() || count->second == 1)
            return full;
    }

    auto baseIt = baseCounts.find(base);
    if (baseIt == baseCounts.end() || baseIt->second <= 1)
        return base;

    return base + "|@" + std::to_string(childIndex);
}

static void assign_child_keys(Element& parent, const std::string& parentKey) {
    std::map<std::string, int> baseCounts;
    std::map<std::string, int> hwndCounts;
    std::map<std::string, int> nameCounts;

    for (const auto& child : parent.children) {
        auto base = base_identity_key(child);
        baseCounts[base]++;
        if (child.nativeHandle != 0)
            hwndCounts[base + "|" + hwnd_key(child.nativeHandle)]++;
        auto name = stable_name_key(child);
        if (!name.empty())
            nameCounts[base + "|" + name]++;
    }

    for (size_t i = 0; i < parent.children.size(); ++i) {
        auto& child = parent.children[i];
        auto compact = compact_instance_key(child);
        if (!compact.empty()) {
            child.key = std::move(compact);
        } else {
            auto segment = discriminator_for_child(child, i, baseCounts, hwndCounts, nameCounts);
            child.key = parentKey.empty() ? segment : parentKey + "/" + segment;
        }
        assign_child_keys(child, child.key);
    }
}

void assign_element_keys(Element& root) {
    root.key = compact_instance_key(root);
    if (root.key.empty())
        root.key = base_identity_key(root);
    assign_child_keys(root, root.key);
}

} // namespace lvt
