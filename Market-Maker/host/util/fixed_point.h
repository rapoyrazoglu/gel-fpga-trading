// MMX — host/util/fixed_point.h
// Q1.31 helpers (spec §6).

#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mmx {

// Convert a real value in [-1.0, +1.0) into Q1.31. Out-of-range values are
// clamped to the representable bounds (the host always clamps before DMA).
inline int32_t to_q31(double x) {
    constexpr double kScale = 2147483648.0;  // 2^31
    constexpr double kMaxIn = 0.9999999995343387;  // (2^31 - 1) / 2^31
    constexpr double kMinIn = -1.0;
    if (!(x == x)) return 0;  // NaN guard
    x = std::clamp(x, kMinIn, kMaxIn);
    double scaled = std::round(x * kScale);
    // Defensive: given the [kMinIn, kMaxIn] clamp above these branches are
    // unreachable (round(kMaxIn*2^31) == INT32_MAX and round(kMinIn*2^31) ==
    // INT32_MIN exactly), but they keep the cast safe if the clamp invariant
    // ever changes.
    if (scaled >  2147483647.0) return  std::numeric_limits<int32_t>::max();
    if (scaled < -2147483648.0) return  std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(scaled);
}

// Inverse: Q1.31 -> double in [-1.0, +1.0).
inline double from_q31(int32_t q) {
    return static_cast<double>(q) / 2147483648.0;
}

// Convert a price expressed in real units to integer ticks given a tick size.
inline int32_t ticks_from_price(double price, double tick_size) {
    // Reject NaN price and non-positive/NaN tick_size in one expression.
    // !(tick_size > 0.0) is true for 0, negatives, and NaN; the price NaN guard
    // prevents the t==NaN value from reaching the UB static_cast below.
    if (!(price == price) || !(tick_size > 0.0)) return 0;
    double t = std::round(price / tick_size);
    if (t >  std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
    if (t <  std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(t);
}

}  // namespace mmx
