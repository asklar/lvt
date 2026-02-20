#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace lvt {

struct Bounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Element {
    std::string id;
    std::string type;
    std::string framework;
    std::string className;
    std::string text;
    Bounds bounds;
    std::map<std::string, std::string> properties;
    std::vector<Element> children;

    // Opaque handle for provider use (e.g. HWND value)
    uintptr_t nativeHandle = 0;
};

} // namespace lvt
