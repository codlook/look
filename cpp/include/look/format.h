// The language's SINGLE double formatter -- INDEPENDENT of the runtime (DB drivers use it too).
// NaN/Infinity are handled explicitly, otherwise std::to_chars shortest (round-trip-safe,
// locale-independent '.'). The same value gives the SAME text everywhere (to_string, JSON, sqlite/mysql/pg results)
// -> "one structure per concept". The previous 4 separate impls (interpreter to_chars, sqlite %.17g,
// pg precision(17), mysql inline to_chars) were round-trip-safe but produced DIFFERENT strings
// (3.141592653589793 vs 3.1415926535897931) and only this one handled NaN/Inf.
#pragma once
#include <charconv>
#include <cmath>
#include <string>

namespace look {

inline std::string look_format_double(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, res.ptr);
}

} // namespace look
