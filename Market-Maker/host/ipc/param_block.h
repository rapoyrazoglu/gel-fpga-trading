// MMX — host/ipc/param_block.h
// CPU ↔ FPGA shared parameter block (spec §3.2, §7.2).
//
// 64 bytes, alignof 64 — fits a single cache line and a single DMA burst.
// HMAC covers the first 48 bytes (data + reserved). The trailing 16 bytes
// are the HMAC tag itself.
//
// Layout fix vs. spec snippet: the snippet in MMX_SPECIFICATION.md §3.2 lists
// `pad[18]` after `hmac[16]`, which sums to 66 (the static_assert in the spec
// would fail). The intent — confirmed by §7.2 ("HMAC over the first 48 bytes")
// — is 32B fields + 16B reserved + 16B HMAC. We adopt the corrected layout.

#pragma once

#include <cstdint>
#include <cstddef>

namespace mmx {

struct alignas(64) ParamBlock {
    // --- HMAC-protected region (offsets 0..47) ---
    int32_t  inv_skew_q31;   //  0  γ·σ²·(T-t) in Q1.31 fixed-point
    int32_t  half_spread;    //  4  δ/2 in price ticks
    uint32_t max_inv;        //  8  |q| cap; FPGA pulls quotes if exceeded
    uint32_t base_qty;       // 12  default order size
    uint8_t  regime_id;      // 16  0=normal, 1=trend_up, 2=trend_down,
                             //     3=high_vol, 4=event_imminent
    uint8_t  kill_switch;    // 17  1 = cancel-all, no place
    int16_t  bid_skew_ticks; // 18  regime-driven asymmetry, signed
    int16_t  ask_skew_ticks; // 20
    uint16_t requote_ticks;  // 22  |Δprice| threshold to retrigger replace
    uint64_t epoch_ns;       // 24  CPU wall-clock; FPGA staleness check
    uint8_t  reserved[16];   // 32  zero-filled; reserved for future use,
                             //     covered by HMAC
    // --- Authentication tag (offsets 48..63) ---
    uint8_t  hmac[16];       // 48  truncated HMAC-SHA256 over bytes [0,48)
};

static_assert(sizeof(ParamBlock) == 64,
              "ParamBlock must be exactly one cache line");
static_assert(alignof(ParamBlock) == 64,
              "ParamBlock must be cache-line aligned");

// Per-field offsets are part of the CPU↔FPGA wire contract: the RTL decodes the
// block by fixed byte offset. Pin every field so a future reorder that keeps
// sizeof==64 (e.g. shrinking reserved[]) can never silently shift epoch_ns or
// the HMAC tag without failing the build.
static_assert(offsetof(ParamBlock, inv_skew_q31)  ==  0, "offset inv_skew_q31");
static_assert(offsetof(ParamBlock, half_spread)   ==  4, "offset half_spread");
static_assert(offsetof(ParamBlock, max_inv)       ==  8, "offset max_inv");
static_assert(offsetof(ParamBlock, base_qty)      == 12, "offset base_qty");
static_assert(offsetof(ParamBlock, regime_id)     == 16, "offset regime_id");
static_assert(offsetof(ParamBlock, kill_switch)   == 17, "offset kill_switch");
static_assert(offsetof(ParamBlock, bid_skew_ticks)== 18, "offset bid_skew_ticks");
static_assert(offsetof(ParamBlock, ask_skew_ticks)== 20, "offset ask_skew_ticks");
static_assert(offsetof(ParamBlock, requote_ticks) == 22, "offset requote_ticks");
static_assert(offsetof(ParamBlock, epoch_ns)      == 24, "offset epoch_ns");
static_assert(offsetof(ParamBlock, reserved)      == 32, "offset reserved");
static_assert(offsetof(ParamBlock, hmac)          == 48, "offset hmac");

// Byte range covered by the HMAC.
static constexpr std::size_t kParamBlockSignedBytes = 48;

// Regime ids — must match host/threads/pcta_ingest.h table.
enum RegimeId : uint8_t {
    kRegimeRanging       = 0,
    kRegimeTrendUp       = 1,
    kRegimeTrendDown     = 2,
    kRegimeHighVol       = 3,
    kRegimeEventImminent = 4,
};

// FPGA staleness threshold — if (now_ns − epoch_ns) > this, FPGA auto-kills.
// 500 ms per spec §7.3.
static constexpr uint64_t kStalenessLimitNs = 500'000'000ULL;

}  // namespace mmx
