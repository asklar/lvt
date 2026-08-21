#pragma once
#include "../element.h"
#include <Windows.h>
#include <optional>
#include <string>
#include <vector>

namespace lvt {

// Inject the WPF TAP DLL into a target process via CreateRemoteThread+LoadLibrary,
// collect the WPF visual tree via the managed WpfTreeWalker, and graft it into
// the element tree.
// Returns true if the tree was successfully enriched.
bool inject_and_collect_wpf_tree(Element& root, HWND hwnd, DWORD pid);

// Parses a WpfTreeWalker JSON payload (an array of Window roots, or a single
// object) into Element trees, with no process injection involved. Exposed so
// the JSON->Element mapping - in particular zeroSize, wpf.visibility, and
// text-absent-vs-empty handling - can be unit tested directly; production
// code reaches this through inject_and_collect_wpf_tree.
//
// Returns std::nullopt if the JSON fails to parse - distinct from a valid
// but empty tree (e.g. "[]"), which returns an empty vector. Collapsing the
// two into the same "empty vector" result is exactly the kind of "silence
// vs. real failure" ambiguity the rest of this header exists to avoid
// elsewhere, so it is not repeated here.
std::optional<std::vector<Element>> wpf_parse_tree_json(const std::string& jsonText,
                                                          const std::string& framework);

} // namespace lvt
