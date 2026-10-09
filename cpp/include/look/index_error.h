#pragma once
// LOOK 2 — indeksleme kuralı ve hata metni: iki motor için TEK tanım.
//
// Kural (Go'daki map okuma gibi, tek cümle):
//   * dizide/map'te olmayan ANAHTAR  → null
//   * null'ı indekslemek             → null   (null boş bir map gibi okunur; Go'da nil map okumak
//                                              hata değildir). `$row["address"]["city"] ?? ""`
//                                              bu yüzden kendiliğinden çalışır — `??` için özel
//                                              bir anlam yoktur.
//   * dizi, map ya da struct OLMAYAN başka bir değeri (metin, sayı, bool, fonksiyon) indekslemek
//                                    → HATA   (bu eksik bir değer değil, yanlış türdür)
// LOOK 1'de yorumlayıcı null dahil hepsinde hata veriyor, VM hepsinde sessizce null dönüyordu:
// aynı satır motora göre farklı davranıyor, yanlış tür null olarak yayılıyordu.
#include "look/interpreter.h"
#include <string>

namespace look {

inline std::string index_error_message(const Value& target, const Value& key) {
    const char* kind =
        target.type() == Value::STRING ? "a string" :
        target.type() == Value::INT    ? "an integer" :
        target.type() == Value::FLOAT  ? "a float" :
        target.type() == Value::BOOL   ? "a boolean" : "a value that is not an array";
    const std::string k = key.type() == Value::STRING ? "\"" + key.to_string() + "\"" : key.to_string();
    return "Index operator requires an array: cannot read [" + k + "] of " + kind
         + " — only arrays, maps and structs can be indexed (a missing key and null both read as null)";
}

} // namespace look
