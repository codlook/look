// Text → double, the single definition used wherever LOOK reads a number from text (json::decode,
// float conversion, numeric checks, database floats, templates). It follows Go's strconv.ParseFloat:
// a value too small for a double (1e-400) is the nearest value (0, or a subnormal such as 5e-324);
// only a value too large (1e309) fails. std::stod failed on both — it throws out_of_range on any
// ERANGE — so json::decode returned null for a valid document holding 5e-324, and float("4.9e-324")
// was 0.
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace look {

// Like std::stod: leading whitespace is skipped and *pos (if given) receives the number of characters
// read. Returns false when nothing is a number or the value overflows a double; `out` is then unchanged.
inline bool parse_double(const std::string& s, double& out, size_t* pos = nullptr) {
    const char* p = s.c_str();
    char* end = nullptr;
    int saved = errno;
    errno = 0;
    double d = std::strtod(p, &end);
    bool overflow = errno == ERANGE && std::isinf(d);
    errno = saved;
    if (end == p || overflow) return false;
    if (pos) *pos = (size_t)(end - p);
    out = d;
    return true;
}

}  // namespace look
