#pragma once
#include "element.h"
#include <string>

namespace lvt {

std::string escape_key_part(const std::string& value);
std::string base_identity_key(const Element& el);

// Assigns every element in `root` a durable, self-describing key
// ("framework|className", "/"-joined down from the root, each segment
// disambiguated among just that element's own siblings — by native
// handle first, then a stable name property, then a local sibling index
// only as a last resort). Used by dump/query/UIA output and, via
// watch_diff.cpp's own indexing, by `watch`'s diffing — deliberately the
// SAME algorithm in both places. An earlier version of watch's diffing had
// its OWN, different key algorithm (global, whole-tree identity counts and
// a full root-to-node path as the disambiguator, rather than this
// function's per-parent/local approach) that quietly overrode whatever key
// this function had already assigned upstream. Its global scope meant a
// change anywhere in the tree could shift the path-derived key of every
// duplicate-identity element elsewhere in the tree (Grid/Border/TextBlock/
// ContentPresenter/Rectangle are exactly this in a real XAML tree) on every
// tick, which watch's diffing (matching purely by key) then reported as
// those elements being removed and different ones added in their place —
// reproduced live as "the tree rebuilds while navigating" against Microsoft
// Store's tree. See watch_diff.cpp's own indexing helper for why this is
// still not perfectly stable across an actual reparent, and why that
// trade-off is the right one.
void assign_element_keys(Element& root);

} // namespace lvt
