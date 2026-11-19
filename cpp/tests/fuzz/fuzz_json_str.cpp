// libFuzzer harness — JSON string unescape (json_decode_str). The highest-exposure class:
// request::json reads every API body from an unauthenticated remote client. Covers the \u surrogate
// and escape logic, the trailing-backslash edge, and the UTF-8 repair. ASan+UBSan look for OOB/UB;
// the harness itself checks that an accepted string is always valid UTF-8 (invalid bytes -> U+FFFD).
#include "look/json_str_parse.h"
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <string>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string s((const char*)data, size);
    size_t i = 0;
    std::string r;
    if (look::json_decode_str(s, i, r)) {
        if (i > s.size()) abort();                        // read past the end
        for (size_t k = 0; k < r.size();) {               // the result is valid UTF-8
            size_t n = look::utf8_valid_len(r, k);
            if (n == 0) abort();
            k += n;
        }
    }
    return 0;
}
