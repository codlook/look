// libFuzzer harness — PostgreSQL ErrorResponse parser (pure function).
// Attacker-controlled bytes: the body of an ERR packet from the server (anyone on the network
// can feed it when S4 verify is off). The historical OOB was exactly this class.
//
// Build+run (clang, AlmaLinux 8 fuzz image):
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -I../../include fuzz_pg_parse_error.cpp -o fuzz_pg_parse_error
//   ./fuzz_pg_parse_error -max_len=4096 corpus/
#include "look/pg_parse.h"
#include <cstdint>
#include <cstddef>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> body(data, data + size);
    volatile auto r = look::pg_parse_error(body);  // volatile: optimize-away'i engelle
    (void)r;
    return 0;
}
