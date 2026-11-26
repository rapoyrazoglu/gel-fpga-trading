// MMX — Acceptance test #10 (spec §10):
// Push `event_imminent`; the next param-compiler refresh must emit a block
// with kill_switch = 1.

#include <vector>

#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "host/threads/param_compiler.h"
#include "host/threads/pcta_ingest.h"
#include "test_helpers.h"

int main() {
    std::vector<std::uint8_t> key(32, 0x77);
    mmx::ParamCompiler pc(key);
    mmx::RegimeMailbox mailbox;

    mmx::ParamCompilerInputs in{};
    in.sigma2_per_sec = 1e-6;
    in.kappa          = 1.0;
    in.horizon_sec    = 600.0;
    in.base_gamma     = 0.1;
    in.regime_id      = mmx::kRegimeRanging;
    in.breaker_tripped= false;
    in.max_inv        = 100;
    in.base_qty       = 1;
    in.requote_ticks  = 1;
    in.tick_size      = 1.0;
    in.now_ns         = 1'000'000'000ULL;

    // Baseline: ranging. Kill must NOT be set.
    mmx::ParamBlock p0{};
    MMX_EXPECT(pc.compile(in, &p0));
    MMX_EXPECT_EQ(p0.kill_switch, 0u);

    // Trend up: regime applies skew but does not kill.
    in.regime_id = mmx::kRegimeTrendUp;
    mmx::ParamBlock p1{};
    MMX_EXPECT(pc.compile(in, &p1));
    MMX_EXPECT_EQ(p1.kill_switch, 0u);
    MMX_EXPECT_EQ(p1.bid_skew_ticks, -1);
    MMX_EXPECT_EQ(p1.ask_skew_ticks, +2);

    // Event imminent → kill must be set.
    mailbox.set(mmx::kRegimeEventImminent);
    in.regime_id = mailbox.get();
    in.now_ns   += 100'000'000ULL;
    mmx::ParamBlock p2{};
    pc.compile(in, &p2);
    MMX_EXPECT_EQ(p2.kill_switch, 1u);
    MMX_EXPECT(mmx::verify_param_block(p2, key.data(), key.size()));

    // Going back to ranging clears the kill.
    in.regime_id = mmx::kRegimeRanging;
    in.now_ns   += 100'000'000ULL;
    mmx::ParamBlock p3{};
    MMX_EXPECT(pc.compile(in, &p3));
    MMX_EXPECT_EQ(p3.kill_switch, 0u);

    MMX_OK();
}
