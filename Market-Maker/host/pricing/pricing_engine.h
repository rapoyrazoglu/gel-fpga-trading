// MMX — host/pricing/pricing_engine.h
// Per-tick pricing core (spec §3.1, §3.2, §4.4).
//
// Single source of truth for the tick-to-trade math:
//   r   = s − ((int64_t)q * inv_skew_q31) >> 31
//   bid = r − half_spread − bid_skew_ticks
//   ask = r + half_spread + ask_skew_ticks
//
// This header is reused by:
//   • host code (pure C++ reference + simulator)
//   • Vitis HLS (fpga/hls/pricing_engine_hls.cpp wraps this with #pragmas)
// so divergence between FPGA and host is structurally impossible.
//
// Integer-only by construction: no float, no double, no transcendentals.

#pragma once

#include <cstdint>

#include "host/ipc/param_block.h"

namespace mmx {

struct QuoteOut {
    int32_t  bid;       // bid price in ticks
    int32_t  ask;       // ask price in ticks
    uint32_t qty;       // order size in lots
    uint8_t  cancel_only;  // 1 = cancel-all, do not place
};

// Saturate an int64 to int32 range (HLS: a min/max clamp, no UB).
inline int32_t sat_i32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(v);
}

// Pure function, no side effects, single-cycle on FPGA.
// Inputs: live mid `s` (ticks), inventory `q` (signed lots), param block.
// Output: bid/ask/qty/cancel_only.
inline QuoteOut compute_quote(int32_t s, int32_t q, const ParamBlock& p) {
    QuoteOut out{};

    // Inventory cap evaluated in the int32 domain via an int64 compare so an
    // "unlimited" max_inv > INT32_MAX (or INT32_MIN negate) never wraps.
    const int64_t cap = static_cast<int64_t>(p.max_inv);
    const uint8_t kill = p.kill_switch
                       | static_cast<uint8_t>(
                             (static_cast<int64_t>(q) >  cap) ||
                             (static_cast<int64_t>(q) < -cap));

    // Clamp inventory to the cap BEFORE the skew multiply, so a stale/garbage q
    // can never drive the skew (and the int64 product) out of a sane range.
    int64_t qc = static_cast<int64_t>(q);
    if (qc >  cap) qc =  cap;
    if (qc < -cap) qc = -cap;

    // r = s - round((q * inv_skew_q31) / 2^31).
    // Pre-add 2^30 before the arithmetic right shift to round-to-nearest
    // instead of flooring toward -inf (which biased r by +0.5 tick). One add
    // + one shift, no divide: HLS-friendly and float-free.
    const int64_t prod = qc * static_cast<int64_t>(p.inv_skew_q31);
    const int64_t skew = (prod + (1LL << 30)) >> 31;
    const int64_t r    = static_cast<int64_t>(s) - skew;

    // Form bid/ask in int64 and saturate to int32 — no signed-overflow UB on
    // host, no silent wrap in RTL.
    out.bid         = sat_i32(r - static_cast<int64_t>(p.half_spread)
                                - static_cast<int64_t>(p.bid_skew_ticks));
    out.ask         = sat_i32(r + static_cast<int64_t>(p.half_spread)
                                + static_cast<int64_t>(p.ask_skew_ticks));
    out.qty         = p.base_qty;
    out.cancel_only = kill;
    return out;
}

// Cancel/replace decision (spec §3.5). FPGA holds (bid_old, ask_old,
// last_quote_ns) in registers; this is the policy in pure form.
struct OutstandingQuote {
    int32_t  bid_old;
    int32_t  ask_old;
    uint64_t last_quote_ns;
};

inline bool needs_replace(const QuoteOut& fresh,
                          const OutstandingQuote& old,
                          uint64_t now_ns,
                          uint16_t requote_ticks,
                          uint64_t max_quote_age_ns,
                          int32_t  q,
                          uint32_t max_inv,
                          uint8_t  kill_switch) {
    auto absdiff = [](int32_t a, int32_t b) -> int32_t {
        return a > b ? a - b : b - a;
    };
    const int64_t cap = static_cast<int64_t>(max_inv);
    if (kill_switch) return true;
    if (static_cast<int64_t>(q) >  cap) return true;
    if (static_cast<int64_t>(q) < -cap) return true;
    // Guard the unsigned subtraction: a clock-skewed now_ns < last_quote_ns
    // would otherwise wrap to ~2^64 and force a spurious replace.
    if (now_ns >= old.last_quote_ns &&
        now_ns - old.last_quote_ns > max_quote_age_ns) return true;
    if (absdiff(fresh.bid, old.bid_old) > static_cast<int32_t>(requote_ticks))
        return true;
    if (absdiff(fresh.ask, old.ask_old) > static_cast<int32_t>(requote_ticks))
        return true;
    return false;
}

}  // namespace mmx
