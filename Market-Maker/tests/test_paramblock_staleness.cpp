// MMX — Acceptance test #6 (spec §10, §7.3):
// A ParamBlock with epoch_ns 600 ms in the past must trigger the FPGA's
// auto-kill on load.

#include <vector>

#include "host/ipc/fpga_stub.h"
#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "test_helpers.h"

int main() {
    std::vector<std::uint8_t> key(32, 0x9c);
    mmx::FpgaStub fpga(key.data(), key.size());

    const std::uint64_t now_ns   = 1'000'000'000ULL;
    const std::uint64_t old_epoch = now_ns - 600'000'000ULL;  // 600 ms old

    mmx::ParamBlock p{};
    p.inv_skew_q31 = 1;
    p.half_spread  = 1;
    p.max_inv      = 1;
    p.base_qty     = 1;
    p.requote_ticks= 1;
    p.epoch_ns     = old_epoch;
    mmx::sign_param_block(p, key.data(), key.size());

    MMX_EXPECT(!fpga.load(p, now_ns));            // load fails, kill asserted
    auto out = fpga.tick(/*s=*/100, /*q=*/0, now_ns);
    MMX_EXPECT_EQ(out.cancel_only, 1u);

    // A fresh block within the window must succeed.
    mmx::ParamBlock fresh{};
    fresh.inv_skew_q31 = 1;
    fresh.half_spread  = 1;
    fresh.max_inv      = 1;
    fresh.base_qty     = 1;
    fresh.requote_ticks= 1;
    fresh.epoch_ns     = now_ns - 100'000'000ULL;
    mmx::sign_param_block(fresh, key.data(), key.size());
    MMX_EXPECT(fpga.load(fresh, now_ns));

    // …but if too long passes since that load, the per-tick gate must kill.
    auto stale_tick = fpga.tick(100, 0, fresh.epoch_ns + 600'000'000ULL);
    MMX_EXPECT_EQ(stale_tick.cancel_only, 1u);

    MMX_OK();
}
