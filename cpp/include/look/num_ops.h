// Sayısal yerleşiklerin ortak çekirdeği — abs / floor / ceil / round TEK tanım (önce üç ayrı kopya
// vardı: stdlib.cpp math::, cli_runtime.cpp, http_main.cpp). Kural LOOK'un taşma kuralıyla aynı:
// int64'e sığan sonuç tamsayı, sığmayan ondalık. ESKİ (UBSan ile ölçüldü): std::abs(INT64_MIN) negatif
// taşma, (int64_t)std::floor(1e300) aralık dışı dönüşüm — ikisi de C++'ta TANIMSIZ davranış.
#pragma once
#include <cmath>
#include <cstdint>
#include "look/value.h"

namespace look {

// Tam sayıya yuvarlanmış double → int64'e sığıyorsa INT, sığmıyorsa (ya da NaN/Inf) FLOAT.
inline Value num_integral_value(double d) {
    if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
        return Value((int64_t)d);
    return Value(d);
}

inline Value num_abs(const Value& v) {
    if (v.type() == Value::FLOAT) return Value(std::fabs(v.as_float()));
    int64_t i = v.to_int();
    if (i == INT64_MIN) return Value(9223372036854775808.0);   // -INT64_MIN int64'e sığmaz
    return Value(i < 0 ? -i : i);
}

inline Value num_floor(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::floor(v.to_float())); }
inline Value num_ceil(const Value& v)  { return v.type() == Value::INT ? v : num_integral_value(std::ceil(v.to_float())); }
inline Value num_round(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::round(v.to_float())); }

} // namespace look
