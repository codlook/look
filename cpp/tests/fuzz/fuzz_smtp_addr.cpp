// libFuzzer harness — SMTP MAIL FROM/RCPT TO address parsing (network-facing, PRE-AUTH).
// Past bugs (bare address, <>, nested '<') are documented in the code; this parser is now bounds-checked.
// Fuzzer: there must be NO CRASH (OOB/segfault) on a random/adversarial line — the return value is irrelevant.
//
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -I../../include fuzz_smtp_addr.cpp -o fuzz_smtp_addr
#include "look/smtp_parse.h"
#include <string>
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string line(reinterpret_cast<const char*>(data), size);
    std::string addr;
    volatile bool ok = look::smtp_extract_addr(line, addr);
    (void)ok;
    volatile auto v = look::smtp_verb(line);
    (void)v;
    return 0;
}
