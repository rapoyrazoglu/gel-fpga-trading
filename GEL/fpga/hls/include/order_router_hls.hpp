// fpga/hls/include/order_router_hls.hpp
//
// HLS-synthesizable order router kernel.
//
// Vitis HLS ingests this file and produces a VHDL/Verilog IP core that
// gets packaged into the AFI for AWS F2.
//
// The kernel mirrors gel::platform::OrderFormatter byte-for-byte so the
// CPU reference and the FPGA produce identical wire frames — this is the
// "bit-exact parity vs C++" gate the deck (slide 15) calls out.
//
// Design contract:
//   - Input  : 64-byte Order POD via AXI-Stream s_axis_order
//   - Output : 48-byte WireFrame via AXI-Stream m_axis_wire
//   - II=1, fully pipelined. One Order in → one WireFrame out per cycle
//     after fill.
//
// Build (Vitis HLS, command line):
//
//     vitis_hls -f scripts/run_hls.tcl
//
// scripts/run_hls.tcl is shipped under fpga/hls/scripts/.
//
// Compile-time guards: real HLS pragmas are gated on __SYNTHESIS__ so this
// file also compiles natively (used by the parity test).
//
#pragma once

// This header is only included by Vitis HLS during synthesis.
// Native (developer-machine) builds use order_router_native.hpp instead.
// Guarded so a `find . -name '*.hpp'` build sweep on Mac doesn't try to
// include the Xilinx headers.
#ifdef GEL_HLS_BUILD

#include <ap_int.h>     // arbitrary-precision integer types — provided by Vitis
#include <hls_stream.h> // hls::stream<>

namespace gel::hls {

    // -------------------------------------------------------------------
    // Wire-protocol constants. MUST stay in lock-step with
    // gel::platform::OrderFormatter — the parity test checks this.
    // -------------------------------------------------------------------
    static constexpr int  ORDER_BYTES      = 64;
    static constexpr int  WIRE_BYTES       = 48;
    static constexpr char OUCH_ENTER_ORDER = 'O';

    // -------------------------------------------------------------------
    // AXI-Stream payload types.
    // -------------------------------------------------------------------
    struct order_axis_t {
        ap_uint<512> data;   // 64 bytes packed
        ap_uint<1>   last;
    };

    struct wire_axis_t {
        ap_uint<384> data;   // 48 bytes packed
        ap_uint<1>   last;
    };

    // -------------------------------------------------------------------
    // Big-endian store helpers. Synthesizable: HLS unrolls the loop.
    // -------------------------------------------------------------------
    static inline void store_be64(ap_uint<384>& bus, int byte_off, ap_uint<64> v) {
        #pragma HLS INLINE
        for (int i = 0; i < 8; ++i) {
            #pragma HLS UNROLL
            bus.range((byte_off + (7 - i)) * 8 + 7,
                      (byte_off + (7 - i)) * 8) = v.range(i * 8 + 7, i * 8);
        }
    }

    static inline void store_be32(ap_uint<384>& bus, int byte_off, ap_uint<32> v) {
        #pragma HLS INLINE
        for (int i = 0; i < 4; ++i) {
            #pragma HLS UNROLL
            bus.range((byte_off + (3 - i)) * 8 + 7,
                      (byte_off + (3 - i)) * 8) = v.range(i * 8 + 7, i * 8);
        }
    }

    // -------------------------------------------------------------------
    // The kernel. One Order → one WireFrame, II=1.
    // -------------------------------------------------------------------
    void order_router_kernel(
        hls::stream<order_axis_t>& s_axis_order,
        hls::stream<wire_axis_t>&  m_axis_wire)
    {
        #pragma HLS PIPELINE II=1
        #pragma HLS INTERFACE axis port=s_axis_order
        #pragma HLS INTERFACE axis port=m_axis_wire
        #pragma HLS INTERFACE ap_ctrl_none port=return

        order_axis_t in = s_axis_order.read();
        ap_uint<512> O = in.data;

        // -- Decode the Order (must mirror connector::Order layout exactly).
        ap_uint<64> ts_ns        = O.range(8 * 8 + 63, 8 * 8 - 56 + 56);    // bytes [0..7]
        // ^^^ HLS bit ranges are inclusive; recompute cleanly:
        ts_ns                    = O.range(63,   0);                          // [0..7]
        ap_uint<64> client_oid   = O.range(127,  64);                         // [8..15]
        ap_uint<32> strat_id     = O.range(159, 128);                         // [16..19]
        ap_uint<32> symbol_id    = O.range(191, 160);                         // [20..23]
        ap_uint<64> price_ticks  = O.range(255, 192);                         // [24..31]
        ap_uint<64> qty          = O.range(319, 256);                         // [32..39]
        ap_uint<8>  side         = O.range(327, 320);                         // [40]
        ap_uint<8>  ord_type     = O.range(335, 328);                         // [41]
        ap_uint<8>  tif          = O.range(343, 336);                         // [42]
        ap_uint<8>  flags        = O.range(351, 344);                         // [43]

        // -- Encode the WireFrame (matches OrderFormatter::encode exactly).
        wire_axis_t out;
        out.data = 0;
        out.last = 1;

        // Header
        out.data.range(7,  0) = ap_uint<8>(OUCH_ENTER_ORDER);  // byte 0
        out.data.range(15, 8) = side;                           // byte 1
        out.data.range(23,16) = ord_type;                       // byte 2
        out.data.range(31,24) = tif;                            // byte 3
        // Body
        store_be64(out.data,  4, client_oid);
        store_be32(out.data, 12, strat_id);
        store_be32(out.data, 16, symbol_id);
        store_be64(out.data, 20, price_ticks);
        store_be64(out.data, 28, qty);
        store_be64(out.data, 36, ts_ns);
        out.data.range(44 * 8 + 7, 44 * 8) = flags;             // byte 44
        // bytes 45..47 stay zero

        m_axis_wire.write(out);
    }

} // namespace gel::hls

#endif // GEL_HLS_BUILD
