#pragma once
// Hash for std::unordered_map<std::string, ...> whose keys can come from a request (cache keys, queue names).
//
// The C++ library's std::hash<std::string> has a fixed seed and its steps can be reversed: keys that share one
// hash value can be built, they land in one bucket, and every call walks the whole chain (hash flooding; 30,000 such
// keys took 15.9 s in cache::set). This is SipHash-1-3 with a per-process random 128-bit seed (as in Rust/Python):
// keys that share a hash value cannot be produced without knowing the seed.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <string>

namespace look {

namespace key_hash_detail {
struct SipKey { uint64_t k0, k1; };
inline const SipKey& sip_key() {
    // No fallback to a constant when the seed cannot be obtained: a predictable hash = hash flooding returns.
    static const SipKey k = [] {
        try {
            std::random_device rd;
            auto w = [&] { return ((uint64_t)rd() << 32) ^ (uint64_t)rd(); };
            return SipKey{ w(), w() };
        } catch (const std::exception& e) {
            std::fprintf(stderr, "LOOK cannot start: no source of random numbers for the hash key (%s)\n", e.what());
            std::abort();
        }
    }();
    return k;
}
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
}  // namespace key_hash_detail

struct KeyHash {
    size_t operator()(const std::string& str) const noexcept {
        using namespace key_hash_detail;
        const SipKey& key = sip_key();
        uint64_t v0 = 0x736f6d6570736575ULL ^ key.k0, v1 = 0x646f72616e646f6dULL ^ key.k1;
        uint64_t v2 = 0x6c7967656e657261ULL ^ key.k0, v3 = 0x7465646279746573ULL ^ key.k1;
        const unsigned char* s = reinterpret_cast<const unsigned char*>(str.data());
        const size_t n = str.size(), full = n & ~(size_t)7;
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
        return (size_t)(v0 ^ v1 ^ v2 ^ v3);
    }
};

}  // namespace look
