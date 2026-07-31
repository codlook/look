// libFuzzer harness — PostgreSQL ErrorResponse ayrıştırıcısı (saf fonksiyon).
// Saldırgan-kontrollü baytlar: sunucudan gelen ERR paketi gövdesi (S4 verify kapalı
// olduğunda ağdaki herkes besleyebilir). Tarihsel OOB tam bu sınıftaydı.
//
// Derle+koş (clang, AlmaLinux 8 fuzz imaji):
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
