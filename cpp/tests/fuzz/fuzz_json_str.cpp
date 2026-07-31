// libFuzzer harness — JSON string-unescape (json_decode_str). EN YUKSEK maruziyet sinifi:
// request::json her API govdesini kimliksiz uzak istemciden isler. \u surrogate + escape
// mantigi + trailing-backslash sinir durumu. ASan+UBSan altinda OOB/UB ara.
#include "look/json_str_parse.h"
#include <cstdint>
#include <cstddef>
#include <string>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string s((const char*)data, size);
    size_t i = 0;
    volatile bool ok = true;
    { std::string r = look::json_decode_str(s, i); (void)r; }
    (void)ok;
    return 0;
}
