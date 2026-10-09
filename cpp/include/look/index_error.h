#pragma once
// LOOK 2 — dizi olmayan bir değeri indeksleme hatası: iki motor için TEK metin.
//
// Kural: bir dizide/map'te olmayan ANAHTARI okumak null verir; dizi, map ya da struct OLMAYAN
// bir değeri (null, metin, sayı, bool, fonksiyon) indekslemek HATADIR. Yorumlayıcı baştan beri
// hata veriyordu, VM sessizce null dönüyordu: aynı satır motora göre farklı davranıyor,
// `$satir["a"]["b"]` gibi bir zincirde tür hatası fark edilmeden null olarak yayılıyordu.
// Mesaj belgedeki başlığı korur (aranabilir) ve çözümü gösterir.
#include "look/interpreter.h"
#include <string>

namespace look {

inline std::string index_error_message(const Value& target, const Value& key) {
    const char* kind =
        target.type() == Value::NONE   ? "null" :
        target.type() == Value::STRING ? "a string" :
        target.type() == Value::INT    ? "an integer" :
        target.type() == Value::FLOAT  ? "a float" :
        target.type() == Value::BOOL   ? "a boolean" : "a value that is not an array";
    const std::string k = key.type() == Value::STRING ? "\"" + key.to_string() + "\"" : key.to_string();
    return "Index operator requires an array: cannot read [" + k + "] of " + kind
         + " — check the value first, or give it a default: ($value ?? [])[" + k + "]";
}

} // namespace look
