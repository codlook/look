// Sayısal yerleşiklerin ortak çekirdeği — abs / floor / ceil / round TEK tanım (önce üç ayrı kopya
// vardı: stdlib.cpp math::, cli_runtime.cpp, http_main.cpp). abs(INT64_MIN) taşma hatası (aritmetikle
// aynı kural); floor/ceil/round ondalık girdide int64'e sığan sonucu tamsayı, sığmayanı ondalık verir.
// ESKİ (UBSan ile ölçüldü): std::abs(INT64_MIN) negatif taşma, (int64_t)std::floor(1e300) aralık dışı
// dönüşüm — ikisi de C++'ta TANIMSIZ davranış.
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
    if (i == INT64_MIN) look_int_overflow(0, "-", i);   // -INT64_MIN int64'e sığmaz: taşma (aritmetikle aynı kural)
    return Value(i < 0 ? -i : i);
}

inline Value num_floor(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::floor(v.to_float())); }
inline Value num_ceil(const Value& v)  { return v.type() == Value::INT ? v : num_integral_value(std::ceil(v.to_float())); }
inline Value num_round(const Value& v) { return v.type() == Value::INT ? v : num_integral_value(std::round(v.to_float())); }

// max/min'in TEK tanımı (çekirdek max()/min() ve math::max/min). Tek liste → en büyük/küçük eleman,
// boş liste → null. ESKİ: çekirdek max([3,9,2]) listenin KENDİSİNİ döndürüyordu (math::max 9) —
// sessiz yanlış sonuç. Birden çok argüman: aralarından seçer. Eşitlikte ilk değil SON geçen döner
// (max: >=, min: <=) — math::max'in eski davranışı korunur. Sözlük çağıranın denetimindedir
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
