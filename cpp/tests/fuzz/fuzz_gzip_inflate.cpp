// libFuzzer harness — gzip response decode (look::gzip_inflate, miniz tinfl underneath).
// Exposure: every http::get / http::post response from a server the app does not control is
// decoded here (Accept-Encoding: gzip is sent by default). This is the client path of the
// Node2Shell / HTTP2Shell class (CVE-2026-111497: a bundled decompressor writing out of bounds
// on a crafted stream). ASan+UBSan: out-of-bounds, use of uninitialised memory; the fuzzer's
// timeout catches a decoder that loops on a malformed stream (CVE-2018-12913 class in miniz).
#include "look/gzip_inflate.h"
#include <cstddef>
#include <cstdint>
#include <string>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string in(reinterpret_cast<const char*>(data), size);
    std::string out;
    volatile bool ok = look::gzip_inflate(in, 1u << 20, out);   // 1 MB cap (bomb guard also fuzzed)
    (void)ok;
    return 0;
}
