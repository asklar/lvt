#pragma once
#include "element.h"
#include <Windows.h>
#include <string>

namespace lvt {

// Capture a screenshot of the given window and save as PNG.
// If tree is provided, overlay bounding boxes and element IDs.
// If elementId is non-empty, crop to that element's bounds.
bool capture_screenshot(HWND hwnd, const std::string& outputPath,
                        const Element* tree = nullptr,
                        const std::string& elementId = {});

} // namespace lvt
