// include/gel/platform/order_formatter.hpp
//
// Block ⑤ in deck slide 6 — Order → wire bytes.
//
// Today: a stub that fills a fixed-layout binary frame. The on-FPGA
// version does the same thing in VHDL with no host CPU involvement.
//
// We pick a NASDAQ-OUCH-flavored layout: fixed-size, big-endian fields.
// The point isn't OUCH compliance — it's having a deterministic, bit-
// exact byte stream that VHDL parity tests can compare against.
//
#pragma once

#include "connector/wire.hpp"
#include "connector/order_router_native.hpp"
#include <array>
#include <cstdint>
#include <cstring>

namespace gel::platform {

    using connector::Order;

    // Wire frame is 56 bytes — matches the connector wire format (see
    // connector/order_router_native.hpp): 48 OUCH-style header + 8-byte
    // ref_order_id at bytes 48-55. The message-type byte (0) is 'O' ENTER /
    // 'X' CANCEL / 'U' REPLACE, derived from ord_type, so CANCEL/REPLACE carry
    // the original coid in ref_order_id and are distinguishable on the wire.
    // (account_id is host-side routing only and does NOT ride on the wire.)
    inline constexpr std::size_t WIRE_FRAME_BYTES = connector::WIRE_BYTES; // 56
    using WireFrame = std::array<uint8_t, WIRE_FRAME_BYTES>;

    namespace detail {
        inline void store_be64(uint8_t* p, uint64_t v) noexcept {
            for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v & 0xff); v >>= 8; }
        }
        inline void store_be32(uint8_t* p, uint32_t v) noexcept {
            for (int i = 3; i >= 0; --i) { p[i] = uint8_t(v & 0xff); v >>= 8; }
        }
    }

    class OrderFormatter {
    public:
        // Pure function: same Order in → same bytes out, every time.
        static WireFrame encode(const Order& o) noexcept {
            using namespace detail;
            WireFrame f{};
            // Header — message type from ord_type: 'O' ENTER / 'X' CANCEL / 'U' REPLACE
            f[0] = static_cast<uint8_t>(connector::ouch_msg_type(o.ord_type));
            f[1] = static_cast<uint8_t>(o.side);
            f[2] = static_cast<uint8_t>(o.ord_type);
            f[3] = static_cast<uint8_t>(o.tif);
            // Body (unchanged for ENTER)
            store_be64(&f[4],  o.client_order_id);
            store_be32(&f[12], o.strategy_id);
            store_be32(&f[16], o.symbol_id);
            store_be64(&f[20], static_cast<uint64_t>(o.price_ticks));
            store_be64(&f[28], static_cast<uint64_t>(o.quantity));
            store_be64(&f[36], o.ts_ns);
            f[44] = o.flags;
            // f[45..47] reserved
            // ref_order_id: original coid for CANCEL/REPLACE, 0 for ENTER.
            store_be64(&f[48], o.ref_order_id);
            return f;
        }
    };

} // namespace gel::platform
