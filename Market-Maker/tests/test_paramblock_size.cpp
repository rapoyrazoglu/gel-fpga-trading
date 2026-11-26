// MMX — Acceptance test #1 (spec §10):
// sizeof(ParamBlock) == 64 && alignof(ParamBlock) == 64.

#include "host/ipc/param_block.h"
#include "test_helpers.h"

int main() {
    static_assert(sizeof(mmx::ParamBlock) == 64,
                  "ParamBlock must be exactly 64 bytes");
    static_assert(alignof(mmx::ParamBlock) == 64,
                  "ParamBlock must be 64-byte aligned");

    // Runtime check too — guards against future ABI surprises.
    MMX_EXPECT_EQ(sizeof(mmx::ParamBlock), std::size_t{64});
    MMX_EXPECT_EQ(alignof(mmx::ParamBlock), std::size_t{64});
    MMX_EXPECT_EQ(mmx::kParamBlockSignedBytes, std::size_t{48});

    // Field offsets should match the spec layout.
    mmx::ParamBlock p{};
    auto base = reinterpret_cast<std::uintptr_t>(&p);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.inv_skew_q31)    - base, 0u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.half_spread)     - base, 4u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.max_inv)         - base, 8u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.base_qty)        - base, 12u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.regime_id)       - base, 16u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.kill_switch)     - base, 17u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.bid_skew_ticks)  - base, 18u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.ask_skew_ticks)  - base, 20u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.requote_ticks)   - base, 22u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.epoch_ns)        - base, 24u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.reserved[0])     - base, 32u);
    MMX_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&p.hmac[0])         - base, 48u);

    MMX_OK();
}
