#pragma once
#include "element.h"
#include "framework_detector.h"
#include <vector>

namespace lvt {

// Build a unified visual tree from the given HWND using detected frameworks.
Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks, int maxDepth = -1);

// Assign deterministic element IDs (e0, e1, ...) in depth-first order.
void assign_element_ids(Element& root);

// Trim element tree to a maximum depth (0 = root only, 1 = root + children, etc.)
void trim_to_depth(Element& root, int maxDepth);

} // namespace lvt
