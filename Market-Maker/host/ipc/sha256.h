// MMX — host/ipc/sha256.h
// FIPS 180-4 SHA-256, self-contained, no external deps.

#pragma once

#include <cstdint>
#include <cstddef>

namespace mmx {

class Sha256 {
  public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t len);
    void finalize(std::uint8_t out[32]);

    // One-shot helper.
    static void hash(const void* data, std::size_t len, std::uint8_t out[32]);

  private:
    void transform(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint64_t bitlen_;
    std::uint8_t  buf_[64];
    std::size_t   buflen_;
};

}  // namespace mmx
