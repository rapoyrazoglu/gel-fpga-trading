// MMX — host/threads/param_compiler.cpp

#include "param_compiler.h"

#include <algorithm>   // std::max — was relied on transitively via fixed_point.h
#include <cmath>
#include <cstring>

#include "host/ipc/hmac.h"
#include "host/util/fixed_point.h"

namespace mmx {

bool ParamCompiler::compile(const ParamCompilerInputs& in,
                            ParamBlock* out) const {
    if (out == nullptr) return false;

    const RegimeOverride ro = regime_table_lookup(in.regime_id);
    const double gamma  = in.base_gamma * ro.gamma_multiplier;
    const double sigma2 = std::max(0.0, in.sigma2_per_sec);
    const double kappa  = std::max(1e-6, in.kappa);
    const double Tt     = std::max(0.0, in.horizon_sec);

    const double inv_skew = gamma * sigma2 * Tt;
    // Guard the log1p argument: a tiny kappa makes gamma/kappa explode (and can
    // overflow to inf), so clamp the ratio to a sane max before the log.
    const double gk_ratio = std::min(gamma / kappa, 1e6);
    const double half_sp  = 0.5 * gamma * sigma2 * Tt
                          + std::log1p(gk_ratio) / std::max(gamma, 1e-9);

    // inv_skew is stored in Q1.31, which only represents [-1, 1). If the
    // computed coefficient reaches/exceeds that range, to_q31() silently
    // saturates and a risk-blind (under-skewing) parameter would reach the
    // chip. Detect it and force a kill rather than ship a clamped coefficient.
    constexpr double kQ31Max = 0.9999999995343387;  // (2^31 - 1) / 2^31
    const bool inv_saturated = (inv_skew >= kQ31Max) || (inv_skew < -1.0);

    ParamBlock p{};
    p.inv_skew_q31    = to_q31(inv_skew);
    p.half_spread     = ticks_from_price(half_sp, in.tick_size);
    p.max_inv         = in.max_inv;
    p.base_qty        = in.base_qty;
    p.regime_id       = in.regime_id;
    p.kill_switch     = (in.breaker_tripped || ro.force_kill || inv_saturated)
                            ? 1u : 0u;
    p.bid_skew_ticks  = ro.bid_skew_ticks;
    p.ask_skew_ticks  = ro.ask_skew_ticks;
    p.requote_ticks   = in.requote_ticks;
    p.epoch_ns        = in.now_ns;
    std::memset(p.reserved, 0, sizeof(p.reserved));

    sign_param_block(p, hmac_key_.data(), hmac_key_.size());

    *out = p;
    // A saturated inv_skew is shipped as a kill block but compile() reports
    // failure so the slow path knows the coefficient was unrepresentable.
    return validate_bounds(p) && !inv_saturated;
}

bool ParamCompiler::validate_bounds(const ParamBlock& p) {
    // half_spread: must be non-negative and reasonable (< 1<<28 ticks).
    if (p.half_spread < 0)         return false;
    if (p.half_spread > (1 << 28)) return false;
    if (p.base_qty == 0)           return false;
    if (p.max_inv == 0)            return false;
    return true;
}

}  // namespace mmx
