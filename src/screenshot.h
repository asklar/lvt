#pragma once
#include "element.h"
#include <Windows.h>
#include <string>
#include <vector>

namespace lvt {

#ifndef NDEBUG
// An annotated element: its id and the pixel-space rectangle drawn on the screenshot.
struct AnnotationInfo {
    std::string id;
    int x, y, width, height;
};
#endif

// Capture a screenshot of the given window and save as PNG.
// If tree is provided, overlay bounding boxes and element IDs.
// If elementId is non-empty, crop to that element's bounds.
bool capture_screenshot(HWND hwnd, const std::string& outputPath,
                        const Element* tree = nullptr,
                        const std::string& elementId = {});

#ifndef NDEBUG
// Compute which elements would be annotated on a screenshot and return their
// pixel-space rectangles. Does NOT capture a frame or produce an image.
std::vector<AnnotationInfo> collect_annotations(HWND hwnd, const Element* tree);
#endif

} // namespace lvt
