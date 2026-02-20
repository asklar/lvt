#pragma once
#include "element.h"
#include "framework_detector.h"
#include <vector>

namespace lvt {

// Build a unified visual tree from the given HWND using detected frameworks.
Element build_tree(HWND hwnd, const std::vector<FrameworkInfo>& frameworks, int maxDepth = -1);

// Assign deterministic element IDs (e0, e1, ...) in depth-first order.
void assign_element_ids(Element& root);

} // namespace lvt
