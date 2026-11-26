// MMX — host/ipc/hmac.cpp

#include "hmac.h"
#include "sha256.h"

#include <cstring>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace mmx {
namespace {

constexpr std::size_t kBlockSize = 64;  // SHA-256 block size

}  // namespace

void hmac_sha256(const void* key, std::size_t key_len,
                 const void* data, std::size_t len,
                 std::uint8_t out[32]) {
    std::uint8_t k_block[kBlockSize] = {};

    if (key_len > kBlockSize) {
        // Long keys are first hashed down (RFC 2104).
        Sha256::hash(key, key_len, k_block);
    } else {
        std::memcpy(k_block, key, key_len);
    }

    std::uint8_t ipad[kBlockSize];
    std::uint8_t opad[kBlockSize];
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        ipad[i] = k_block[i] ^ 0x36;
        opad[i] = k_block[i] ^ 0x5c;
    }

    std::uint8_t inner[32];
    {
        Sha256 h;
        h.update(ipad, kBlockSize);
        h.update(data, len);
        h.finalize(inner);
    }
    {
        Sha256 h;
        h.update(opad, kBlockSize);
        h.update(inner, 32);
        h.finalize(out);
    }
}

void sign_param_block(ParamBlock& block,
                      const void* key, std::size_t key_len) {
    // Zero hmac field before signing so callers can call sign() on a partially
    // populated block without leaking stale tag bytes into the digest input
    // (the signed range is [0, 48), but be tidy).
    std::memset(block.hmac, 0, sizeof(block.hmac));

    std::uint8_t tag[32];
    hmac_sha256(key, key_len, &block, kParamBlockSignedBytes, tag);
    std::memcpy(block.hmac, tag, sizeof(block.hmac));
}

bool verify_param_block(const ParamBlock& block,
                        const void* key, std::size_t key_len) {
    std::uint8_t tag[32];
    hmac_sha256(key, key_len, &block, kParamBlockSignedBytes, tag);

    // Constant-time compare on the 16 truncated bytes.
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < sizeof(block.hmac); ++i) {
        diff |= static_cast<std::uint8_t>(tag[i] ^ block.hmac[i]);
    }
    return diff == 0;
}

std::vector<std::uint8_t> load_hmac_key(const std::string& path) {
    constexpr std::size_t kRequired = 32;

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::runtime_error("load_hmac_key: stat failed for '" + path + "'");
    }
    if (static_cast<std::size_t>(st.st_size) != kRequired) {
        throw std::runtime_error(
            "load_hmac_key: '" + path + "' must be exactly 32 bytes (got " +
            std::to_string(st.st_size) + ")");
    }
#if defined(S_IRWXG) && defined(S_IRWXO)
    // Reject world- or group-readable key files.
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error(
            "load_hmac_key: '" + path + "' must have mode 0600 (no group/world perms)");
    }
#endif

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("load_hmac_key: open failed for '" + path + "'");
    }
    std::vector<std::uint8_t> key(kRequired);
    f.read(reinterpret_cast<char*>(key.data()), static_cast<std::streamsize>(kRequired));
    if (!f || f.gcount() != static_cast<std::streamsize>(kRequired)) {
        throw std::runtime_error("load_hmac_key: short read for '" + path + "'");
    }
    return key;
}

std::size_t hex_decode(std::string_view hex,
                       std::uint8_t* out, std::size_t out_cap) {
    if (hex.size() % 2 != 0) return 0;
    if (hex.size() / 2 > out_cap) return 0;
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nyb(hex[i]);
        int lo = nyb(hex[i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return hex.size() / 2;
}

}  // namespace mmx
