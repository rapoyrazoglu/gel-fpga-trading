// connector/order_router_native.hpp
//
// Native (non-HLS) shim of the FPGA order_router kernel.
//
// Mirrors the body of order_router_hls.hpp's order_router_kernel() but
// works on plain byte buffers — no ap_int<>, no hls::stream<>. Used by
// the bit-exact parity test on a regular developer machine.
//
// Anything that changes in the HLS kernel MUST be mirrored here, and
// vice-versa. The parity test will catch a mismatch immediately.
//
// Wire format (56 B, OUCH-flavored, big-endian on the wire):
//
//    0     msg type: 'O' ENTER (MARKET..POST_ONLY) / 'X' CANCEL / 'U' REPLACE
//    1     side
//    2     ord_type
//    3     tif
//    4-11  client_order_id        (REPLACE: the fresh coid)
//   12-15  strategy_id
//   16-19  symbol_id
//   20-27  price_ticks            (CANCEL: -1, ignored)
//   28-35  quantity               (CANCEL: ignored)
//   36-43  ts_ns
//   44     flags
//   45-47  reserved
//   48-55  ref_order_id           (CANCEL/REPLACE: original coid; 0 for ENTER)
//
// Note: account_id (host-side routing tag) is NOT carried on the wire —
// the broker doesn't see it. It only travels in the connector::Order POD.
//
#pragma once

#include "wire.hpp"

#include <cstdint>
#include <cstring>

namespace connector {

    inline constexpr int  ORDER_BYTES        = 64;
    inline constexpr int  WIRE_BYTES         = 56;   // 48 B ENTER body + 8 B ref_order_id
    inline constexpr char OUCH_ENTER_ORDER   = 'O';
    inline constexpr char OUCH_CANCEL_ORDER  = 'X';
    inline constexpr char OUCH_REPLACE_ORDER = 'U';

    // OUCH message type from ord_type: ENTER variants (MARKET..POST_ONLY) send
    // 'O', CANCEL sends 'X', REPLACE sends 'U'. Same mapping in both encoders
    // and (when ported) the FPGA order_encoder kernel.
    inline constexpr char ouch_msg_type(uint8_t ord_type) noexcept {
        return ord_type == static_cast<uint8_t>(OrdType::CANCEL)  ? OUCH_CANCEL_ORDER
             : ord_type == static_cast<uint8_t>(OrdType::REPLACE) ? OUCH_REPLACE_ORDER
             :                                                      OUCH_ENTER_ORDER;
    }

    // Encode a 64-byte Order POD (little-endian as on the host) into a
    // 56-byte wire frame (big-endian, OUCH-flavored layout). All fields
    // including ref_order_id are encoded unconditionally (ENTER uses
    // ref_order_id=0). Pure function.
    inline void encode_native(const uint8_t order_bytes[ORDER_BYTES],
                              uint8_t       wire_bytes [WIRE_BYTES]) noexcept {
        std::memset(wire_bytes, 0, WIRE_BYTES);

        auto rd_le64 = [&](int off) -> uint64_t {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= uint64_t(order_bytes[off + i]) << (8 * i);
            return v;
        };
        auto rd_le32 = [&](int off) -> uint32_t {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= uint32_t(order_bytes[off + i]) << (8 * i);
            return v;
        };
        auto wr_be64 = [&](int off, uint64_t v) {
            for (int i = 7; i >= 0; --i) { wire_bytes[off + i] = uint8_t(v & 0xff); v >>= 8; }
        };
        auto wr_be32 = [&](int off, uint32_t v) {
            for (int i = 3; i >= 0; --i) { wire_bytes[off + i] = uint8_t(v & 0xff); v >>= 8; }
        };

        wire_bytes[0] = uint8_t(ouch_msg_type(order_bytes[41]));  // 'O'/'X'/'U'
        wire_bytes[1] = order_bytes[40];               // side
        wire_bytes[2] = order_bytes[41];               // ord_type
        wire_bytes[3] = order_bytes[42];               // tif
        wr_be64(4,  rd_le64(8));                       // client_order_id
        wr_be32(12, rd_le32(16));                      // strategy_id
        wr_be32(16, rd_le32(20));                      // symbol_id
        wr_be64(20, rd_le64(24));                      // price_ticks
        wr_be64(28, rd_le64(32));                      // quantity
        wr_be64(36, rd_le64(0));                       // ts_ns
        wire_bytes[44] = order_bytes[43];              // flags
        wr_be64(48, rd_le64(52));                      // ref_order_id (0 for ENTER)
    }

    // Reference encoder — same wire format expressed via direct field
    // access on the Order struct. The parity test asserts that
    // encode_native (byte-buffer path) and encode_reference (struct path)
    // produce the same 56-byte frame for every Order. If they diverge,
    // either the HLS shim or the host-side encoder is wrong.
    inline void encode_reference(const Order& o,
                                 uint8_t      wire_bytes[WIRE_BYTES]) noexcept {
        std::memset(wire_bytes, 0, WIRE_BYTES);

        auto wr_be64 = [&](int off, uint64_t v) {
            for (int i = 7; i >= 0; --i) { wire_bytes[off + i] = uint8_t(v & 0xff); v >>= 8; }
        };
        auto wr_be32 = [&](int off, uint32_t v) {
            for (int i = 3; i >= 0; --i) { wire_bytes[off + i] = uint8_t(v & 0xff); v >>= 8; }
        };

        wire_bytes[0] = uint8_t(ouch_msg_type(o.ord_type));     // 'O'/'X'/'U'
        wire_bytes[1] = o.side;
        wire_bytes[2] = o.ord_type;
        wire_bytes[3] = o.tif;
        wr_be64(4,  o.client_order_id);
        wr_be32(12, o.strategy_id);
        wr_be32(16, o.symbol_id);
        wr_be64(20, static_cast<uint64_t>(o.price_ticks));
        wr_be64(28, static_cast<uint64_t>(o.quantity));
        wr_be64(36, o.ts_ns);
        wire_bytes[44] = o.flags;
        wr_be64(48, o.ref_order_id);                            // 0 for ENTER
    }

} // namespace connector
