// Standalone driver for the libFuzzer harnesses when clang/libFuzzer is not available (local g++
// build with -fsanitize=address,undefined). Not coverage-guided: it replays the seed corpus, then
// runs random mutations (bit flips, byte sets, inserts, deletes, splices of seeds, interesting
// lengths) for the given number of seconds. A timeout per input catches a decoder that loops.
// Usage: <binary> <corpus_dir> <seconds> [seed]
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static std::string g_cur;
static void on_alarm(int) {
    FILE* f = std::fopen("timeout-input.bin", "wb");
    if (f) { std::fwrite(g_cur.data(), 1, g_cur.size(), f); std::fclose(f); }
    std::fprintf(stderr, "TIMEOUT: one input ran over 5 s (saved to timeout-input.bin, %zu bytes)\n", g_cur.size());
    std::_Exit(70);
}
static void run(const std::string& s) {
    g_cur = s;
    alarm(5);
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    alarm(0);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <corpus_dir> <seconds> [seed]\n", argv[0]); return 2; }
    std::signal(SIGALRM, on_alarm);
    std::vector<std::string> seeds;
    for (auto& e : std::filesystem::directory_iterator(argv[1])) {
        std::ifstream f(e.path(), std::ios::binary);
        seeds.emplace_back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    if (seeds.empty()) seeds.emplace_back("");
    for (auto& s : seeds) run(s);
    std::mt19937_64 rng(argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 12345);
    auto pick = [&](size_t n) { return n ? (size_t)(rng() % n) : 0; };
    const uint64_t magic[] = {0, 1, 0x7f, 0x80, 0xff, 0x7e, 0x7fff, 0xffff, 0x10000, 0x7fffffff, 0xffffffff};
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(argv[2]));
    unsigned long long n = 0;
    while (std::chrono::steady_clock::now() < end) {
        std::string s = seeds[pick(seeds.size())];
        int muts = 1 + (int)pick(8);
        for (int m = 0; m < muts; ++m) {
            switch (pick(7)) {
                case 0: if (!s.empty()) s[pick(s.size())] ^= (char)(1u << pick(8)); break;
                case 1: if (!s.empty()) s[pick(s.size())] = (char)rng(); break;
                case 2: s.insert(pick(s.size() + 1), 1 + pick(16), (char)rng()); break;
                case 3: if (!s.empty()) { size_t a = pick(s.size()); s.erase(a, 1 + pick(std::min<size_t>(32, s.size() - a))); } break;
                case 4: { auto& o = seeds[pick(seeds.size())]; if (!o.empty()) s.insert(pick(s.size() + 1), o.substr(pick(o.size()))); } break;
                case 5: if (s.size() >= 4) { uint64_t v = magic[pick(sizeof(magic) / sizeof(magic[0]))]; size_t a = pick(s.size() - 3);
                            for (int k = 0; k < 4; ++k) s[a + k] = (char)(v >> (8 * k)); } break;
                case 6: if (!s.empty()) s.resize(pick(s.size() + 1)); break;
            }
        }
        if (s.size() > (1u << 20)) s.resize(1u << 20);
        run(s);
        ++n;
    }
    std::printf("standalone fuzz: %llu inputs, no crash\n", n);
    return 0;
}
