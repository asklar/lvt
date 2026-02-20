#pragma once
#include "../element.h"
#include <Windows.h>

namespace lvt {

// Base interface for visual tree providers.
class IProvider {
public:
    virtual ~IProvider() = default;
};

} // namespace lvt
