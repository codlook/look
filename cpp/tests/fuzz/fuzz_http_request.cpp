// libFuzzer harness — HTTP/1.1 request/header parser (pure function).
// HIGHEST EXPOSURE: these bytes are PRE-AUTH — anyone on the network (before authentication)
// feeds them. request-line splitting, path/query, header key/value trim, bare-CR/LF scan,
// obs-fold rejection, duplicate Content-Length, Upgrade/Accept detection — all here.
// Goal: prove there is no OOB/UB/crash (substr offset arithmetic + [] accesses).
//
// Build+run (clang):
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
    volatile int ok = look::http_parse_request(raw, req);  // volatile: optimize-away engelle
    (void)ok;
    return 0;
}
