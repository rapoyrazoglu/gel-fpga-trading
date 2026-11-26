// MMX — host/threads/pcta_ingest.h
// Slow-path regime ingest from PCTA (spec §5.4). Latest value wins.

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "host/ipc/param_block.h"

namespace mmx {

struct RegimeOverride {
    int16_t bid_skew_ticks;
    int16_t ask_skew_ticks;
    double  gamma_multiplier;
    bool    force_kill;
};

inline RegimeOverride regime_table_lookup(uint8_t regime_id) {
    switch (regime_id) {
        case kRegimeRanging:       return { 0,  0, 1.0, false};
        case kRegimeTrendUp:       return {-1, +2, 1.2, false};
        case kRegimeTrendDown:     return {-2, +1, 1.2, false};
        case kRegimeHighVol:       return {-1, +1, 2.5, false};
        case kRegimeEventImminent: return { 0,  0, 1.0, true };
        default:                   return { 0,  0, 1.0, false};
    }
}

// Map a string regime name (e.g. "high_vol") to the canonical id. Unknown
// strings map to kRegimeRanging.
uint8_t parse_regime_name(std::string_view s);

// Mailbox: thread-safe latest-value-wins handoff for the regime hint.
class RegimeMailbox {
  public:
    RegimeMailbox() : value_(kRegimeRanging) {}
    void   set(uint8_t regime_id) { value_.store(regime_id, std::memory_order_release); }
    uint8_t get() const           { return value_.load(std::memory_order_acquire); }
  private:
    std::atomic<uint8_t> value_;
};

}  // namespace mmx
