#pragma once
#include "element.h"
#include <cstdint>
#include <string>

namespace lvt {

struct CompactXamlKey {
    std::string framework;
    uint64_t handle = 0;
};

std::string escape_key_part(const std::string& value);
std::string base_identity_key(const Element& el);

// Parse a process-wide XAML diagnostics identity produced by
// assign_element_keys. Structural and non-XAML keys cannot address an object
// for IVisualTreeService property operations and are rejected explicitly.
bool parse_compact_xaml_key(const std::string& text, CompactXamlKey& out,
                            std::string& error);

// A stable, human-meaningful identifier for `el` drawn from the first of
// AutomationId / x:Name / Name (and their lowercase property-bag spellings)
// that is actually present, or empty if none are. Exposed (not file-local)
// so watch_diff.cpp's cross-tick reconciliation can use the exact same
// notion of "this child is identifiable independent of its position" that
// assign_element_keys' own per-sibling disambiguation already uses —
// having two different answers to "is this child otherwise identifiable"
// living in two different files is exactly the kind of divergence that
// caused this area's bugs before.
std::string stable_name_key(const Element& el);

// Assigns every element in `root` a durable, self-describing key. XAML and
// WinUI3 elements with an IXamlDiagnostics InstanceHandle use the compact,
// process-wide form "xaml:0x..." / "winui3:0x...". Other providers (and the
// rare XAML node without a handle) use "framework|className" path segments,
// "/"-joined from the nearest structural ancestor and disambiguated among
// siblings by fixed-width provider handle first, native handle second, then a
// stable name property, then a local sibling index as a last resort. Used by
// dump/query/UIA output, and by
// watch_diff.cpp to give a *freshly discovered* element (the first tick, or
// a genuinely new node appearing later) its initial key.
//
// watch's diffing does NOT rely on recomputing this same key fresh on
// every tick to recognize a persisting element as "the same one" —a
// fallback structural key, with no memory between ticks, cannot do that
// robustly: it threads every ancestor's own disambiguating segment into
// each descendant's key, so if *any* ancestor's position among its own
// same-identity siblings shifts for any reason (extremely common in a
// live, animated UI — carousels, virtualized lists recycling items), every
// element beneath it gets a brand-new key on that tick even though nothing
// about it individually changed. Verified live: a passively-watched,
// completely untouched Microsoft Store home page (whose carousel
// auto-rotates) showed nearly its *entire* tree repeatedly flip-flopping
// between removed and re-added, purely from time passing.
//
// Instead, watch_diff.cpp's own reconciliation matches each tick's tree
// against the previous tick's node-by-node (by native handle, then
// stable_name_key, then relative position among remaining same-identity
// siblings) and has a matched node INHERIT its predecessor's key outright,
// never recomputing it. A key assigned by this function only "sticks" for
// as long as reconciliation keeps recognizing that same conceptual slot —
// which is indefinitely, unless the element is actually removed.
void assign_element_keys(Element& root);

} // namespace lvt
