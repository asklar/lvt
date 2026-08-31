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
    std::string key;
    std::string type;
    std::string framework;
    std::string className;
    std::string text;
    Bounds bounds;
    std::map<std::string, std::string> properties;
    std::vector<Element> children;

    // Native pointer-sized identity such as HWND.
    uintptr_t nativeHandle = 0;

    // Public durable identity for native HWND keys. This is separate from
    // providerHandle so read-only/cross-process HWNDs can retain stable keys
    // even when they are not eligible for mutation registration.
    uint64_t nativeLifetimeHandle = 0;

    // Framework/provider object identity. Kept separately because some
    // providers use fixed-width wire handles that are wider than uintptr_t on
    // x86 (XAML InstanceHandle is MIDL_uhyper).
    uint64_t providerHandle = 0;

    // Provider-supplied, public-safe logical identity used only for durable
    // key construction. This is deliberately separate from providerHandle:
    // native property adapters may need opaque, session-local mutation
    // handles that must never make the public key connection-dependent.
    std::string durableIdentity;
};

} // namespace lvt
