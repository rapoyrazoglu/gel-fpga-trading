// MMX — host/ipc/sha256.cpp
// FIPS 180-4 SHA-256.

#include "sha256.h"

#include <algorithm>
#include <cstring>

namespace mmx {
namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

}  // namespace

void Sha256::reset() {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    bitlen_ = 0;
    buflen_ = 0;
}

void Sha256::transform(const std::uint8_t block[64]) {
    std::uint32_t W[64];
    for (int i = 0; i < 16; ++i) {
        W[i] = (std::uint32_t(block[i * 4]) << 24) |
               (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) |
                std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
        std::uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch  = (e & f) ^ ((~e) & g);
        std::uint32_t T1  = h + S1 + ch + K[i] + W[i];
        std::uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t T2  = S0 + maj;
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bitlen_ += static_cast<std::uint64_t>(len) * 8;
    while (len > 0) {
        std::size_t take = std::min<std::size_t>(64 - buflen_, len);
        std::memcpy(buf_ + buflen_, p, take);
        buflen_ += take;
        p += take;
        len -= take;
        if (buflen_ == 64) {
            transform(buf_);
            buflen_ = 0;
        }
    }
}

void Sha256::finalize(std::uint8_t out[32]) {
    // Append 0x80, then zero-pad until 56 mod 64, then 8-byte BE length.
    buf_[buflen_++] = 0x80;
    if (buflen_ > 56) {
        while (buflen_ < 64) buf_[buflen_++] = 0;
        transform(buf_);
        buflen_ = 0;
    }
    while (buflen_ < 56) buf_[buflen_++] = 0;
    const std::uint64_t bl = bitlen_;
    for (int i = 0; i < 8; ++i) {
        buf_[56 + i] = static_cast<std::uint8_t>(bl >> ((7 - i) * 8));
    }
    transform(buf_);

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<std::uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    reset();
}

void Sha256::hash(const void* data, std::size_t len, std::uint8_t out[32]) {
    Sha256 h;
    h.update(data, len);
    h.finalize(out);
}

}  // namespace mmx
