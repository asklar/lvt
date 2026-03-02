#pragma once
#include <cmath>
#include <climits>
#include <optional>

namespace lvt {

// Safe double-to-int conversion: clamp to int range and reject non-finite values.
// Returns std::nullopt for NaN/Infinity so callers can skip bounds entirely.
// Prevents undefined behavior from static_cast<int> when the double is NaN, Inf,
// or outside the representable int range.
inline std::optional<int> safe_double_to_int(double v) {
    if (!std::isfinite(v)) return std::nullopt;
    if (v >= static_cast<double>(INT_MAX)) return INT_MAX;
    if (v <= static_cast<double>(INT_MIN)) return INT_MIN;
    return static_cast<int>(v);
}

} // namespace lvt
