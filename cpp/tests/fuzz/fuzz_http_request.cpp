// libFuzzer harness — HTTP/1.1 istek/başlık ayrıştırıcısı (saf fonksiyon).
// EN YÜKSEK MARUZİYET: bu baytlar PRE-AUTH — ağdaki herkes (kimlik doğrulamadan önce)
// besler. request-line bölme, path/query, header key/value trim, çıplak-CR/LF taraması,
// obs-fold reddi, çift Content-Length, Upgrade/Accept algılama — hepsi burada.
// Amaç: OOB/UB/çökme yok mu kanıtla (substr offset aritmetiği + [] erişimleri).
//
// Derle+koş (clang):
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -fno-sanitize-recover=all -I../../include fuzz_http_request.cpp -o fuzz_http_request
//   ./fuzz_http_request -max_len=8192 corpus_http/
#include "look/http_parse.h"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string raw(reinterpret_cast<const char*>(data), size);
    look::HttpRequest req;
    volatile bool ok = look::http_parse_request(raw, req);  // volatile: optimize-away engelle
    (void)ok;
    return 0;
}
