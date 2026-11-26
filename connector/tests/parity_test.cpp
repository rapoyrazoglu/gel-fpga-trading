// tests/parity_test.cpp
//
// Bit-exact parity test — gates VHDL bring-up.
//
// The FPGA implementation must produce the same wire bytes as the host
// reference for every Order. We have two encoders in the connector:
//
//   - encode_reference  — struct-typed, easy to read, the "correct" one
//   - encode_native     — byte-buffer port of the HLS kernel; this is
//                         the path that must match VHDL bit-for-bit
//
// Asserting they agree on a fuzzed corpus catches regressions in either
// direction. When VHDL/HLS comes online, drop in its byte-buffer output
// in place of encode_native and the same test gates the synthesis bring-up.
//
#include "connector/order_router_native.hpp"
#include "connector/wire.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <random>

using connector::Order;
using connector::WIRE_BYTES;
using connector::encode_native;
using connector::encode_reference;

namespace {

Order make_random_order(std::mt19937_64& rng) {
    Order o{};
    o.ts_ns           = rng();
    o.client_order_id = rng();
    o.strategy_id     = static_cast<uint32_t>(rng() % 256);
    o.symbol_id       = static_cast<uint32_t>(rng() % 4096);
    o.price_ticks     = static_cast<int64_t>(rng());
    o.quantity        = static_cast<int64_t>(rng());
    o.side            = uint8_t(rng() % 2);
    o.ord_type        = uint8_t(rng() % 7);   // 0..6: ENTER variants + CANCEL + REPLACE
    o.tif             = uint8_t(rng() % 4);
    o.flags           = uint8_t(rng() & 0xff);
    o.account_id      = static_cast<uint32_t>(rng() % 16);
    o.ref_order_id    = rng();                // exercised on CANCEL/REPLACE frames
    return o;
}

void serialize_order(const Order& o, uint8_t buf[64]) {
    static_assert(sizeof(Order) == 64);
    std::memcpy(buf, &o, sizeof(o));
}

void hex_dump(const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        std::printf("%02x ", p[i]);
        if ((i + 1) % 16 == 0) std::printf("\n");
    }
    std::printf("\n");
}

} // namespace

int main() {
    std::mt19937_64 rng{0xCAFEF00DDEADBEEFULL};
    constexpr int N = 100'000;

    int  failures        = 0;
    Order first_fail_ord = {};

    for (int i = 0; i < N; ++i) {
        const Order o = make_random_order(rng);

        uint8_t order_bytes[64];
        serialize_order(o, order_bytes);

        uint8_t native_frame[WIRE_BYTES];
        uint8_t ref_frame   [WIRE_BYTES];
        encode_native   (order_bytes, native_frame);
        encode_reference(o,           ref_frame);

        if (std::memcmp(native_frame, ref_frame, WIRE_BYTES) != 0) {
            if (failures == 0) first_fail_ord = o;
            ++failures;
        }
    }

    // --- L2 semantic check: the encoder must actually DIFFERENTIATE message
    //     types (O/X/U) and carry ref_order_id — self-agreement isn't enough. --
    int sem_fail = 0;
    auto check_type = [&](connector::OrdType ot, char want, uint64_t ref) {
        Order o{};
        o.ord_type        = static_cast<uint8_t>(ot);
        o.client_order_id = 0xAABBCCDD11223344ULL;
        o.ref_order_id    = ref;
        uint8_t f[WIRE_BYTES];
        encode_reference(o, f);
        uint64_t wire_ref = 0;
        for (int i = 0; i < 8; ++i) wire_ref = (wire_ref << 8) | f[48 + i];
        if (f[0] != uint8_t(want)) {
            std::printf("SEM FAIL: ord_type %u -> '%c' want '%c'\n",
                        uint8_t(ot), f[0], want); ++sem_fail; }
        if (wire_ref != ref) {
            std::printf("SEM FAIL: ref_order_id 0x%llx -> 0x%llx\n",
                        (unsigned long long)ref, (unsigned long long)wire_ref); ++sem_fail; }
    };
    check_type(connector::OrdType::MARKET,  'O', 0);
    check_type(connector::OrdType::LIMIT,   'O', 0);
    check_type(connector::OrdType::CANCEL,  'X', 0xDEADBEEFCAFE0001ULL);
    check_type(connector::OrdType::REPLACE, 'U', 0x0123456789ABCDEFULL);

    std::printf("\n=== connector parity test ===\n");
    std::printf("Orders tested:  %d\n", N);
    std::printf("Mismatches:     %d\n", failures);
    std::printf("Semantic O/X/U + ref_order_id: %s\n", sem_fail ? "FAIL" : "PASS");

    if (failures == 0 && sem_fail == 0) {
        std::printf("Status:         PASS\n");
        std::printf("=============================\n\n");
        return 0;
    }

    std::printf("Status:         FAIL\n\n");
    uint8_t order_bytes[64];
    serialize_order(first_fail_ord, order_bytes);
    uint8_t native_frame[WIRE_BYTES], ref_frame[WIRE_BYTES];
    encode_native   (order_bytes,    native_frame);
    encode_reference(first_fail_ord, ref_frame);

    std::printf("First failing Order (raw 64 B):\n");
    hex_dump(order_bytes, 64);
    std::printf("Reference frame (56 B):\n");
    hex_dump(ref_frame, WIRE_BYTES);
    std::printf("HLS-shim frame (56 B):\n");
    hex_dump(native_frame, WIRE_BYTES);
    std::printf("=============================\n\n");
    return 1;
}
