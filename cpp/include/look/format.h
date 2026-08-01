// Dilin TEK double formatlayıcısı — interpreter'dan BAĞIMSIZ (DB sürücüleri de kullanır).
// NaN/Infinity açıkça ele alınır, aksi halde std::to_chars shortest (round-trip-güvenli,
// locale-bağımsız '.'). Aynı değer her yerde (to_string, JSON, sqlite/mysql/pg sonuç) AYNI
// metin → "kavram başına tek yapı". Önceki 4 ayrı impl (interpreter to_chars, sqlite %.17g,
// pg precision(17), mysql satır-içi to_chars) round-trip'te güvenliydi ama FARKLI string
// üretiyordu (3.141592653589793 vs 3.1415926535897931) ve NaN/Inf'i yalnız bu ele alıyordu.
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
