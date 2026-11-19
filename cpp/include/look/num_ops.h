// Common core of the numeric built-ins — abs / floor / ceil / round in ONE definition (there used to be three separate copies:
// stdlib.cpp math::, cli_runtime.cpp, http_main.cpp). abs(INT64_MIN) is an overflow error (same rule
// as arithmetic); floor/ceil/round give an integer for a float input if the result fits int64, otherwise a float.
// OLD (measured with UBSan): std::abs(INT64_MIN) negative overflow, (int64_t)std::floor(1e300) out-of-range
// conversion — both are UNDEFINED behaviour in C++.
#pragma once
#include <cmath>
#include <cstdint>
#include "look/value.h"

namespace look {

// A double rounded to an integer: INT if it fits int64, FLOAT if not (or NaN/Inf).
inline Value num_integral_value(double d) {
    if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
        return Value((int64_t)d);
    return Value(d);
}

inline Value num_abs(const Value& v) {
    if (v.type() == Value::FLOAT) return Value(std::fabs(v.as_float()));
    int64_t i = v.to_int();
    if (i == INT64_MIN) look_int_overflow(0, "-", i);   // -INT64_MIN does not fit int64: overflow (same rule as arithmetic)
    return Value(i < 0 ? -i : i);
}

inline Value num_floor(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::floor(v.to_float())); }
inline Value num_ceil(const Value& v)  { return v.type() == Value::INT ? v : num_integral_value(std::ceil(v.to_float())); }
inline Value num_round(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::round(v.to_float())); }

// The SINGLE definition of max/min (core max()/min() and math::max/min). A single list → the largest/smallest element,
// an empty list → null. OLD: core max([3,9,2]) returned the list ITSELF (math::max gave 9) —
// a silent wrong result. Several arguments: picks among them. On a tie the LAST one is returned, not the first
// (max: >=, min: <=) — math::max's old behaviour is preserved. The map is under the caller's control
// (value_list_only).
inline Value num_extreme(const std::vector<Value>& args, bool want_max) {
    auto better = [want_max](const Value& a, const Value& best) { return want_max ? a >= best : a <= best; };
    if (args.size() == 1 && args[0].type() == Value::ARRAY) {
        const auto& arr = *args[0].vec_ptr();
        if (arr.empty()) return Value();
        Value best = arr[0];
        for (size_t i = 1; i < arr.size(); ++i) if (better(arr[i], best)) best = arr[i];
        return best;
    }
    if (args.empty()) return Value();
    Value best = args[0];
    for (size_t i = 1; i < args.size(); ++i) if (better(args[i], best)) best = args[i];
    return best;
}

} // namespace look
