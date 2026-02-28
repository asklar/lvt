#pragma once
#include <cmath>
#include <climits>

namespace lvt {

// Safe double-to-int conversion: clamp to int range and reject non-finite values.
// Prevents undefined behavior from static_cast<int> when the double is NaN, Inf,
// or outside the representable int range.
inline int safe_double_to_int(double v) {
    if (!std::isfinite(v)) return 0;
    if (v >= static_cast<double>(INT_MAX)) return INT_MAX;
    if (v <= static_cast<double>(INT_MIN)) return INT_MIN;
    return static_cast<int>(v);
}

} // namespace lvt
