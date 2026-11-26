// MMX — fpga/hls/pricing_engine_hls.cpp
// Vitis HLS top-level for the pricing engine (spec §4.4).
//
// The math lives in host/pricing/pricing_engine.h — single source of truth,
// shared with the host reference. Here we only add HLS pragmas and the AXI-
// stream-friendly top function. The same translation unit compiles cleanly
// under a stock C++20 compiler so the FPGA path is part of the host CTest
// (no float/double survives — see tests/test_no_float_in_fpga.cpp).
//
// To synthesize:
//   v++ -c --mode hls --part xcvu47p-fsvh2892-2-e \
//       -DMMX_HLS_BUILD pricing_engine_hls.cpp
//
// Pragmas are gated under MMX_HLS_BUILD so a host build sees plain C++.

#include <cstdint>

#include "host/ipc/param_block.h"
#include "host/pricing/pricing_engine.h"

#ifdef MMX_HLS_BUILD
  #define MMX_HLS_PRAGMA(x) _Pragma(#x)
#else
  #define MMX_HLS_PRAGMA(x)
#endif

extern "C" void mmx_pricing_engine(
    const mmx::ParamBlock& p_in,   // BRAM-resident param block
    int32_t                s,      // mid in ticks
    int32_t                q,      // signed inventory
    int32_t&               bid_out,
    int32_t&               ask_out,
    uint32_t&              qty_out,
    uint8_t&               cancel_only_out)
{
    MMX_HLS_PRAGMA(HLS pipeline II=1)
    MMX_HLS_PRAGMA(HLS interface ap_none port=s)
    MMX_HLS_PRAGMA(HLS interface ap_none port=q)
    MMX_HLS_PRAGMA(HLS interface bram   port=p_in)
    MMX_HLS_PRAGMA(HLS interface ap_none port=bid_out)
    MMX_HLS_PRAGMA(HLS interface ap_none port=ask_out)
    MMX_HLS_PRAGMA(HLS interface ap_none port=qty_out)
    MMX_HLS_PRAGMA(HLS interface ap_none port=cancel_only_out)

    mmx::QuoteOut qo = mmx::compute_quote(s, q, p_in);
    bid_out         = qo.bid;
    ask_out         = qo.ask;
    qty_out         = qo.qty;
    cancel_only_out = qo.cancel_only;
}
