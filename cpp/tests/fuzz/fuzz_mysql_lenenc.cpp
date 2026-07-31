// libFuzzer harness — MySQL length-encoded int/str ayrıştırma (tarihsel OOB sınıfı).
// Row decoder deseni: paket gövdesini peş peşe lenenc-str olarak okur (kolon değerleri).
// `read_lenenc_str`'in taşma-clamp'i (len>avail → clamp) OOB'yi engellemeli — kanıtla.
//
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -I../../include fuzz_mysql_lenenc.cpp -o fuzz_mysql_lenenc
#include "look/mysql_wire_parse.h"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1) Satır ayrıştırma: peş peşe lenenc-str (kolon değeri okuma deseni)
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    int guard = 0;
    while (p < end && guard++ < 100000) {
        const uint8_t* before = p;
        volatile auto s = look::mysql_read_lenenc_str(p, end);
        (void)s;
        if (p == before) break;   // ilerleme yok → çık
    }
    // 2) lenenc int ayrı
    const uint8_t* q = data;
    volatile auto n = look::mysql_read_lenenc(q, end);
    (void)n;
    return 0;
}
