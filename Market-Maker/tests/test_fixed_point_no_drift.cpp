// MMX — Acceptance test #7 (spec §10):
// Replay 1e6 random (s, q) tuples and confirm the integer pricing path
// matches a double-precision reference within ±1 tick.

#include <cmath>
#include <cstdint>
#include <random>

#include "host/ipc/param_block.h"
#include "host/pricing/pricing_engine.h"
#include "host/util/fixed_point.h"
#include "test_helpers.h"

int main() {
    mmx::ParamBlock p{};
    const double inv_skew_real = 0.012345;     // γσ²(T-t), within Q1.31
    const double half_sp_real  = 6.0;          // ticks
    p.inv_skew_q31    = mmx::to_q31(inv_skew_real);
    p.half_spread     = static_cast<int32_t>(half_sp_real);
    p.max_inv         = 1'000'000;
    p.base_qty        = 1;
    p.bid_skew_ticks  = 0;
    p.ask_skew_ticks  = 0;
    p.requote_ticks   = 1;
    p.epoch_ns        = 1;

    std::mt19937 rng(0xBEEF);
    std::uniform_int_distribution<int32_t> s_dist(1, 1'000'000);
    std::uniform_int_distribution<int32_t> q_dist(-100'000, 100'000);

    constexpr int N = 1'000'000;
    int max_abs_err_bid = 0, max_abs_err_ask = 0;
    for (int i = 0; i < N; ++i) {
        int32_t s = s_dist(rng);
        int32_t q = q_dist(rng);

        auto out = mmx::compute_quote(s, q, p);

        // Double-precision reference of the same math.
        double r_ref   = static_cast<double>(s) - static_cast<double>(q) * inv_skew_real;
        double bid_ref = r_ref - half_sp_real;
        double ask_ref = r_ref + half_sp_real;

        long long err_bid = std::abs(static_cast<long long>(out.bid) -
                                     static_cast<long long>(std::llround(bid_ref)));
        long long err_ask = std::abs(static_cast<long long>(out.ask) -
                                     static_cast<long long>(std::llround(ask_ref)));
        if (err_bid > max_abs_err_bid) max_abs_err_bid = static_cast<int>(err_bid);
        if (err_ask > max_abs_err_ask) max_abs_err_ask = static_cast<int>(err_ask);
        MMX_EXPECT(err_bid <= 1);
        MMX_EXPECT(err_ask <= 1);
    }

    std::printf("max |bid err| = %d, max |ask err| = %d (over %d samples)\n",
                max_abs_err_bid, max_abs_err_ask, N);
    MMX_OK();
}
