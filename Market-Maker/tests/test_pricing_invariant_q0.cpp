// MMX — Acceptance test #3 (spec §10):
// When q == 0, FPGA-mode pricing returns symmetric quotes around s with
// half-spread = δ/2 exactly.

#include <initializer_list>

#include "host/ipc/param_block.h"
#include "host/pricing/pricing_engine.h"
#include "test_helpers.h"

int main() {
    mmx::ParamBlock p{};
    p.inv_skew_q31    = 100000;
    p.half_spread     = 7;
    p.max_inv         = 10;
    p.base_qty        = 1;
    p.bid_skew_ticks  = 0;
    p.ask_skew_ticks  = 0;
    p.requote_ticks   = 1;
    p.epoch_ns        = 1;
    p.kill_switch     = 0;

    for (int32_t s : {1, 100, 10000, 1'000'000, 1'234'567}) {
        auto out = mmx::compute_quote(s, /*q=*/0, p);
        MMX_EXPECT_EQ(out.bid, s - p.half_spread);
        MMX_EXPECT_EQ(out.ask, s + p.half_spread);
        MMX_EXPECT_EQ(out.bid + out.ask, 2 * s);
        MMX_EXPECT_EQ(out.qty, p.base_qty);
        MMX_EXPECT_EQ(out.cancel_only, 0u);
    }
    MMX_OK();
}
