// libFuzzer harness — SMTP MAIL FROM/RCPT TO adres ayrıştırma (ağa açık, PRE-AUTH).
// Geçmiş bug'lar (bare adres, <>, iç içe '<') kodda belgeli; bu parser artık bounds-checked.
// Fuzzer: rastgele/adversarial satırda ÇÖKME (OOB/segfault) OLMAMALI — dönüş değeri önemsiz.
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
