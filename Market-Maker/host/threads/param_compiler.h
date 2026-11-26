// MMX — host/threads/param_compiler.h
// The slow-path heart (spec §5.3): every ~100 ms snapshot σ², κ, regime,
// compute (inv_skew, half_spread), pack ParamBlock, sign, validate, hand off.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "host/ipc/param_block.h"
#include "host/threads/pcta_ingest.h"

namespace mmx {

struct ParamCompilerInputs {
    double  sigma2_per_sec;     // from EwmaVariance
    double  kappa;              // from KappaEstimator
    double  horizon_sec;        // T - t (wall-clock based)
    double  base_gamma;         // operator-set
    uint8_t regime_id;          // from RegimeMailbox
    bool    breaker_tripped;    // circuit breaker

    uint32_t max_inv;
    uint32_t base_qty;
    uint16_t requote_ticks;
    double   tick_size;         // currency per tick — for half_spread → ticks
    std::uint64_t now_ns;
};

class ParamCompiler {
  public:
    explicit ParamCompiler(std::vector<std::uint8_t> hmac_key)
        : hmac_key_(std::move(hmac_key)) {}

    // Build, sign, and validate a fresh block. Returns true on success and
    // fills *out; false if bounds-check fails (block is left signed-but-flagged).
    bool compile(const ParamCompilerInputs& in, ParamBlock* out) const;

    // Validate computed values fit the fixed-point representation. Public for
    // testing.
    static bool validate_bounds(const ParamBlock& p);

  private:
    std::vector<std::uint8_t> hmac_key_;
};

}  // namespace mmx
