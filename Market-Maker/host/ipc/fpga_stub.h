// MMX — host/ipc/fpga_stub.h
// Software model of the FPGA's `param_block_loader` + tick-side gating
// (spec §4.7, §4.8, §7.3). Used by host-side acceptance tests to exercise
// behaviour that, in production, lives in RTL on the chip.
//
// Keeping the model in C++ lets every Phase A acceptance test run from CTest,
// while the Phase B RTL must implement identical semantics.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#include "param_block.h"
#include "hmac.h"
#include "host/pricing/pricing_engine.h"

namespace mmx {

class FpgaStub {
  public:
    FpgaStub(const std::uint8_t* key, std::size_t key_len)
        : key_(key, key + key_len) {}

    // CPU writes a fresh ParamBlock. FPGA verifies HMAC + staleness; if
    // either fails the active block's kill_switch is asserted.
    bool load(const ParamBlock& candidate, std::uint64_t now_ns) {
        if (!verify_param_block(candidate, key_.data(), key_.size())) {
            assert_kill();
            last_load_error_ = "hmac";
            return false;
        }
        if ((now_ns >= candidate.epoch_ns) &&
            (now_ns - candidate.epoch_ns) > kStalenessLimitNs) {
            assert_kill();
            last_load_error_ = "stale";
            return false;
        }
        active_       = candidate;
        active_valid_ = true;
        return true;
    }

    // Per-tick gate: returns the quote action the FPGA would emit.
    QuoteOut tick(int32_t s, int32_t q, std::uint64_t now_ns) {
        QuoteOut out{};
        // Guard the unsigned subtraction (mirrors load()): a future-dated /
        // clock-skewed epoch_ns must not wrap to ~2^64 and spuriously kill.
        if (!active_valid_ ||
            (now_ns >= active_.epoch_ns &&
             (now_ns - active_.epoch_ns) > kStalenessLimitNs)) {
            out.cancel_only = 1;
            return out;
        }
        // §3.5 / quote_decider.vhd: a resting quote older than MAX_QUOTE_AGE
        // (50 ms) is auto-cancelled. If the last quote we placed has aged past
        // that, pull quotes this cycle and reset the age clock; the next tick
        // re-quotes fresh. (The 500 ms param-block staleness gate above is a
        // separate, coarser check and is preserved.)
        if (have_last_quote_ && now_ns >= last_quote_ns_ &&
            (now_ns - last_quote_ns_) > kMaxQuoteAgeNs) {
            out.cancel_only = 1;
            last_quote_ns_  = now_ns;
            return out;
        }
        out = compute_quote(s, q, active_);
        if (!out.cancel_only) {
            last_quote_ns_   = now_ns;
            have_last_quote_ = true;
        }
        return out;
    }

    bool                 active_valid()   const { return active_valid_; }
    const ParamBlock&    active()         const { return active_; }
    const char*          last_load_error()const { return last_load_error_; }

  private:
    void assert_kill() {
        active_.kill_switch = 1;
    }

    // §3.5 MAX_QUOTE_AGE — a placed quote may rest at most 50 ms before the
    // FPGA auto-cancels it (matches quote_decider.vhd).
    static constexpr std::uint64_t kMaxQuoteAgeNs = 50'000'000ULL;

    std::vector<std::uint8_t> key_;
    ParamBlock                active_{};
    bool                      active_valid_ = false;
    const char*               last_load_error_ = "";
    std::uint64_t             last_quote_ns_ = 0;
    bool                      have_last_quote_ = false;
};

}  // namespace mmx
