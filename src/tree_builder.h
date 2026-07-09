#pragma once
#include "element.h"
#include "framework_detector.h"
#include <optional>
#include <string>
#include <vector>

namespace lvt {

// Build a unified visual tree from the given HWND using detected frameworks.
Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks,
                   int maxDepth = -1, const std::string& pluginOption = {});

// Assign deterministic element IDs (e0, e1, ...) in depth-first order.
void assign_element_ids(Element& root);

// Find an element by its assigned positional ID, durable key, or either (ID first).
Element* find_element_by_id(Element& root, const std::string& id);
const Element* find_element_by_id(const Element& root, const std::string& id);
Element* find_element_by_key(Element& root, const std::string& key);
const Element* find_element_by_key(const Element& root, const std::string& key);
Element* find_element_by_ref(Element& root, const std::string& ref);
const Element* find_element_by_ref(const Element& root, const std::string& ref);

// Return a built-in or dynamic element property value.
std::optional<std::string> get_element_property(const Element& element, const std::string& property);

// Trim element tree to a maximum depth (0 = root only, 1 = root + children, etc.)
void trim_to_depth(Element& root, int maxDepth);

} // namespace lvt
