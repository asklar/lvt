#pragma once
#include "element.h"
#include "framework_detector.h"
#include <optional>
#include <string>
#include <vector>

namespace lvt {

// Build a unified visual tree from the given HWND using detected frameworks.
// `fastProperties` skips IVisualTreeService::GetPropertyValuesChain for
// XAML/WinUI3 elements (the dominant per-element cost of a rich tree) in
// favor of cheaper direct WinRT property reads — see
// xaml_diag_common.h's inject_and_collect_xaml_tree for exactly what that
// trades away. Defaults to false (today's exhaustive property collection),
// unaffected for every non-XAML/WinUI3 provider.
Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks,
                   int maxDepth = -1, const std::string& pluginOption = {},
                   bool fastProperties = false);

// Assign deterministic element IDs (e0, e1, ...) in depth-first order.
void assign_element_ids(Element& root);

// Find an element by its assigned positional ID, durable key, or either (ID first).
Element* find_element_by_id(Element& root, const std::string& id);
const Element* find_element_by_id(const Element& root, const std::string& id);
Element* find_element_by_key(Element& root, const std::string& key);
const Element* find_element_by_key(const Element& root, const std::string& key);

// Resolve an element reference: a positional id ("e4"), a durable key, or a
// "uia:<RuntimeId>" reference against a UIA tree.
Element* find_element_by_ref(Element& root, const std::string& ref);
const Element* find_element_by_ref(const Element& root, const std::string& ref);

// Return a built-in or dynamic element property value.
std::optional<std::string> get_element_property(const Element& element, const std::string& property);

// Trim element tree to a maximum depth (0 = root only, 1 = root + children, etc.)
void trim_to_depth(Element& root, int maxDepth);

} // namespace lvt
