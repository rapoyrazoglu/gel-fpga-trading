// MMX — Acceptance test #2 (spec §10):
// Sign + verify 10 000 random ParamBlocks; zero failures.
// Also verifies SHA-256 and HMAC-SHA-256 against published test vectors:
//   FIPS 180-4 SHA-256 short messages
//   RFC 4231 HMAC-SHA256 test cases 1, 2, 4

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "host/ipc/sha256.h"
#include "test_helpers.h"

static void check_sha256(const char* msg, std::size_t n, const char* expect) {
    std::uint8_t out[32];
    mmx::Sha256::hash(msg, n, out);
    if (!mmx_hex_eq(out, 32, expect)) {
        std::fprintf(stderr, "SHA-256 mismatch for input length %zu\n", n);
        std::exit(2);
    }
}

static void check_hmac(const std::uint8_t* key, std::size_t klen,
                       const std::uint8_t* data, std::size_t dlen,
                       const char* expect) {
    std::uint8_t out[32];
    mmx::hmac_sha256(key, klen, data, dlen, out);
    if (!mmx_hex_eq(out, 32, expect)) {
        std::fprintf(stderr, "HMAC-SHA256 mismatch (klen=%zu, dlen=%zu)\n",
                     klen, dlen);
        std::exit(3);
    }
}

int main() {
    // ---- SHA-256 sanity vectors. ----
    check_sha256("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha256("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha256(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // ---- RFC 4231 HMAC-SHA256 test cases. ----
    {  // case 1
        std::uint8_t key[20];
        std::memset(key, 0x0b, 20);
        const char* data = "Hi There";
        check_hmac(key, 20, reinterpret_cast<const std::uint8_t*>(data), 8,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    {  // case 2
        const char* key  = "Jefe";
        const char* data = "what do ya want for nothing?";
        check_hmac(reinterpret_cast<const std::uint8_t*>(key), 4,
                   reinterpret_cast<const std::uint8_t*>(data), 28,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    {  // case 4
        std::uint8_t key[25];
        for (int i = 0; i < 25; ++i) key[i] = static_cast<std::uint8_t>(i + 1);
        std::uint8_t data[50];
        std::memset(data, 0xcd, 50);
        check_hmac(key, 25, data, 50,
            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    }

    // ---- ParamBlock round-trip on 10 000 random blocks. ----
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::uint8_t> key(32);
    for (auto& b : key) b = static_cast<std::uint8_t>(byte_dist(rng));

    constexpr int N = 10000;
    int verified = 0;
    for (int i = 0; i < N; ++i) {
        mmx::ParamBlock p{};
        p.inv_skew_q31    = static_cast<int32_t>(rng());
        p.half_spread     = static_cast<int32_t>(rng() & 0x0fffffff);
        p.max_inv         = rng() & 0xffff;
        p.base_qty        = (rng() & 0xff) + 1u;
        p.regime_id       = static_cast<std::uint8_t>(rng() % 5);
        p.kill_switch     = static_cast<std::uint8_t>(rng() & 1);
        p.bid_skew_ticks  = static_cast<std::int16_t>(rng() & 0xffff);
        p.ask_skew_ticks  = static_cast<std::int16_t>(rng() & 0xffff);
        p.requote_ticks   = static_cast<std::uint16_t>(rng() & 0xffff);
        p.epoch_ns        = (static_cast<std::uint64_t>(rng()) << 32) | rng();

        mmx::sign_param_block(p, key.data(), key.size());
        if (mmx::verify_param_block(p, key.data(), key.size())) ++verified;
    }
    MMX_EXPECT_EQ(verified, N);

    // Tampered block must NOT verify.
    mmx::ParamBlock t{};
    t.epoch_ns = 12345;
    mmx::sign_param_block(t, key.data(), key.size());
    t.inv_skew_q31 ^= 1;
    MMX_EXPECT(!mmx::verify_param_block(t, key.data(), key.size()));

    // Wrong key must NOT verify.
    mmx::ParamBlock t2{};
    mmx::sign_param_block(t2, key.data(), key.size());
    std::vector<std::uint8_t> wrong = key;
    wrong[0] ^= 0xff;
    MMX_EXPECT(!mmx::verify_param_block(t2, wrong.data(), wrong.size()));

    MMX_OK();
}
