// look::sip13 (include/look/sip13.h) is SipHash-1-3: it gives the published-style test vectors.
//
// The hash of cache keys and queue names is only as good as its code: a line broken later would still compile, still
// work, and quietly make the hash weaker. The vectors below were made by an independent implementation (a Python
// transcription of the SipHash paper) that was first checked against the official SipHash-2-4 vector
// (key 00 01 .. 0f, input 00 01 .. 0e -> a129ca6149be45e5); here the same code runs with 1 compression and
// 3 finalization rounds. Key 00 01 .. 0f (k0 = 0x0706050403020100, k1 = 0x0f0e0d0c0b0a0908), input 00 01 .. (n-1).
// Build: g++ -std=c++17 -Iinclude tests/key_hash_test.cpp -o /tmp/key_hash_test && /tmp/key_hash_test
#include "look/sip13.h"
#include <cstdio>

int main() {
    static const unsigned long long want[17] = {
        0xabac0158050fc4dcULL, 0xc9f49bf37d57ca93ULL, 0x82cb9b024dc7d44dULL, 0x8bf80ab8e7ddf7fbULL,
        0xcf75576088d38328ULL, 0xdef9d52f49533b67ULL, 0xc50d2b50c59f22a7ULL, 0xd3927d989bb11140ULL,
        0x369095118d299a8eULL, 0x25a48eb36c063de4ULL, 0x79de85ee92ff097fULL, 0x70c118c1f94dc352ULL,
        0x78a384b157b4d9a2ULL, 0x306f760c1229ffa7ULL, 0x605aa111c0f95d34ULL, 0xd320d86d2a519956ULL,
        0xcc4fdd1a7d908b66ULL };
    unsigned char in[16];
    for (int i = 0; i < 16; ++i) in[i] = (unsigned char)i;
    int bad = 0;
    for (int n = 0; n <= 16; ++n) {
        unsigned long long got = look::sip13(in, (size_t)n, 0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL);
        if (got != want[n]) { std::printf("FAIL length %d: got %016llx, want %016llx\n", n, got, want[n]); bad = 1; }
    }
    if (!bad) std::printf("PASS: SipHash-1-3 test vectors (17 lengths)\n");
    return bad;
}
