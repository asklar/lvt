#pragma once
#include "provider.h"

namespace lvt {

class Win32Provider : public IProvider {
public:
    // Build the full HWND tree starting from the given root window.
    Element build(HWND hwnd, int maxDepth = -1);

private:
    Element build_element(HWND hwnd, int depth, int maxDepth);
};

} // namespace lvt
