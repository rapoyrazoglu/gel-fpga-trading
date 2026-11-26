// MMX — Acceptance test #4 (spec §10):
// q > 0 ⇒ bid + ask < 2s (skewed down to encourage selling).
// q < 0 ⇒ bid + ask > 2s (skewed up).
//
// Q1.31 caps the per-lot skew at < 1.0 ticks, so q must be large enough that
// q · inv_skew_q31 ≥ 2^31 for the integer skew to be non-zero. We pick
// inv_skew_q31 = 2^30 (= 0.5) and start q at 2.

#include <initializer_list>

#include "host/ipc/param_block.h"
#include "host/pricing/pricing_engine.h"
#include "test_helpers.h"

int main() {
    mmx::ParamBlock p{};
    p.inv_skew_q31    = 1 << 30;   // exactly 0.5 in Q1.31
    p.half_spread     = 5;
    p.max_inv         = 1000;
    p.base_qty        = 1;
    p.bid_skew_ticks  = 0;
    p.ask_skew_ticks  = 0;
    p.requote_ticks   = 1;
    p.epoch_ns        = 1;

    const int32_t s = 100000;

    int32_t prev_pos_sum = 2 * s + 1;  // start above to allow first ≤ to pass
    int32_t prev_neg_sum = 2 * s - 1;

    for (int32_t q : {2, 5, 50, 100, 500}) {
        auto pos = mmx::compute_quote(s,  q, p);
        auto neg = mmx::compute_quote(s, -q, p);
        MMX_EXPECT(pos.bid + pos.ask <  2 * s);
        MMX_EXPECT(neg.bid + neg.ask >  2 * s);

        // Monotonicity in |q|: more inventory ⇒ stronger skew.
        // (Note: signed arithmetic right shift floors toward −∞, so the
        //  absolute skew on the +q and −q sides need not be exactly equal.)
        MMX_EXPECT(pos.bid + pos.ask <= prev_pos_sum);
        MMX_EXPECT(neg.bid + neg.ask >= prev_neg_sum);
        prev_pos_sum = pos.bid + pos.ask;
        prev_neg_sum = neg.bid + neg.ask;
    }
    MMX_OK();
}
