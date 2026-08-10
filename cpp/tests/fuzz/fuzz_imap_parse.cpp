// libFuzzer harness — IMAP komut + mailbox adı ayrıştırma (ağa açık, PRE-AUTH).
// Mailbox guard belgeli geçmiş bug (`INBOX; rm -rf /`) düzeltildi; rastgele girdide ÇÖKME olmamalı.
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -I../../include fuzz_imap_parse.cpp -o fuzz_imap_parse
#include "look/imap_parse.h"
#include <string>
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string line(reinterpret_cast<const char*>(data), size);
    std::string tag, cmd, args; look::imap_parse_command(line, tag, cmd, args);
    std::string a, b;           look::imap_split_two(args, a, b);
    std::string cleaned;        volatile bool ok = look::imap_mailbox_name_ok(line, cleaned); (void)ok;
    return 0;
}
