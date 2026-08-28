#pragma once
#include "provider.h"

namespace lvt {

class NativePropertyConnection;

class Win32Provider : public IProvider {
public:
    // Build the full HWND tree starting from the given root window.
    Element build(
        HWND hwnd, int maxDepth = -1,
        NativePropertyConnection* properties = nullptr);

private:
    Element build_element(
        HWND hwnd, int depth, int maxDepth,
        NativePropertyConnection* properties);
};

} // namespace lvt
