// libFuzzer harness — MySQL length-encoded int/str parsing (historical OOB class).
// Row decoder pattern: reads the packet body as consecutive lenenc-str (column values).
// The overflow clamp of `read_lenenc_str` (len>avail → clamp) must prevent OOB — prove it.
//
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -I../../include fuzz_mysql_lenenc.cpp -o fuzz_mysql_lenenc
#include "look/mysql_wire_parse.h"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1) Row parsing: consecutive lenenc-str (column value reading pattern)
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    int guard = 0;
    while (p < end && guard++ < 100000) {
        const uint8_t* before = p;
        volatile auto s = look::mysql_read_lenenc_str(p, end);
        (void)s;
        if (p == before) break;   // no progress → exit
    }
    // 2) lenenc int separately
    const uint8_t* q = data;
    volatile auto n = look::mysql_read_lenenc(q, end);
    (void)n;
    return 0;
}
