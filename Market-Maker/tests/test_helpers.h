// MMX — tests/test_helpers.h
// Minimal assertion macros — one executable per acceptance test.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#define MMX_FAIL(msg) do { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
    std::exit(1); \
} while (0)

#define MMX_EXPECT(cond) do { \
    if (!(cond)) MMX_FAIL("EXPECT(" #cond ") failed"); \
} while (0)

#define MMX_EXPECT_EQ(a, b) do { \
    auto _aa = (a); auto _bb = (b); \
    if (!(_aa == _bb)) { \
        std::fprintf(stderr, "FAIL %s:%d: EXPECT_EQ(%s, %s)\n", \
                     __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } \
} while (0)

#define MMX_OK() do { \
    std::printf("PASS %s\n", __FILE__); \
    return 0; \
} while (0)

inline bool mmx_hex_eq(const std::uint8_t* bytes, std::size_t n,
                       const char* hex) {
    if (std::strlen(hex) != 2 * n) return false;
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        int hi = nyb(hex[i*2]);
        int lo = nyb(hex[i*2+1]);
        if (hi < 0 || lo < 0) return false;
        if (bytes[i] != static_cast<std::uint8_t>((hi << 4) | lo)) return false;
    }
    return true;
}
