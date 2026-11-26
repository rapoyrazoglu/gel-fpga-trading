// MMX — host/ipc/hmac.h
// HMAC-SHA256 + ParamBlock sign/verify.

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "param_block.h"

namespace mmx {

// 32-byte HMAC-SHA256 over `data[len]` with key `key[key_len]`.
void hmac_sha256(const void* key, std::size_t key_len,
                 const void* data, std::size_t len,
                 std::uint8_t out[32]);

// HMAC the first 48 bytes of `block` using `key`, write the leading 16 bytes
// of the digest into `block.hmac`. Mutates `block` only in the hmac field.
void sign_param_block(ParamBlock& block,
                      const void* key, std::size_t key_len);

// Recompute and constant-time-compare the truncated tag against block.hmac.
bool verify_param_block(const ParamBlock& block,
                        const void* key, std::size_t key_len);

// Small helper for tests / config: hex-decode "DEADBEEF…" to bytes.
// Returns number of bytes written, or 0 on parse error.
std::size_t hex_decode(std::string_view hex,
                       std::uint8_t* out, std::size_t out_cap);

// G11 — operator-side HMAC key provisioning. Reads exactly 32 bytes from
// `path`. On POSIX systems, the file mode must be 0600 (rw owner only) —
// world-readable keys are rejected with std::runtime_error. Throws
// std::runtime_error on any failure (open, size mismatch, mode check).
//
// Production layout: a 32-byte binary file under /etc/mmx/hmac.key managed
// by the deployment tool, never checked into the source tree.
std::vector<std::uint8_t> load_hmac_key(const std::string& path);

}  // namespace mmx
