#pragma once
// SipHash-1-3 (1 compression round, 3 finalization rounds; Rust's HashMap default) of n bytes under the 128-bit
// key (k0, k1). look_key_hash (value.cpp) calls it with the per-process random key; tests/key_hash_test.cpp checks it
// against test vectors, so a broken line cannot weaken the hash unnoticed.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace look {

inline uint64_t sip13(const unsigned char* s, size_t n, uint64_t k0, uint64_t k1) noexcept {
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    const size_t full = n & ~(size_t)7;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m;
        std::memcpy(&m, s + i, 8);   // little-endian machines (x86-64/arm64)
        v3 ^= m; round(); v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t i = 0; i < (n & 7); ++i) b |= (uint64_t)s[full + i] << (8 * i);
    v3 ^= b; round(); v0 ^= b;
    v2 ^= 0xff;
    round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace look
