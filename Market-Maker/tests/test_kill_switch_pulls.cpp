// MMX — Acceptance test #5 (spec §10):
// With kill_switch = 1 the FPGA stub must emit cancel-only and never a place.

#include <initializer_list>
#include <vector>

#include "host/ipc/fpga_stub.h"
#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "test_helpers.h"

int main() {
    std::vector<std::uint8_t> key(32, 0x42);
    mmx::FpgaStub fpga(key.data(), key.size());

    mmx::ParamBlock p{};
    p.inv_skew_q31 = 1 << 28;
    p.half_spread  = 5;
    p.max_inv      = 100;
    p.base_qty     = 1;
    p.kill_switch  = 1;
    p.requote_ticks= 1;
    p.epoch_ns     = 1'000'000'000ULL;
    mmx::sign_param_block(p, key.data(), key.size());

    MMX_EXPECT(fpga.load(p, /*now_ns=*/p.epoch_ns + 1'000'000ULL));

    for (int32_t q : {-50, -1, 0, 1, 50}) {
        auto out = fpga.tick(/*s=*/100000, q, /*now_ns=*/p.epoch_ns + 1'000'000ULL);
        MMX_EXPECT_EQ(out.cancel_only, 1u);
    }
    MMX_OK();
}
