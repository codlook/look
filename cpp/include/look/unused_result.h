#pragma once
// Sonucu kullanılmayan saf yerleşik çağrısı: `array::push($list, $x)` tek başına bir cümle.
// array::push listeyi YERİNDE değiştirmez, yeni liste döndürür — sonuç atılınca hiçbir şey olmaz,
// hata da çıkmaz (VS Code örneğinde not hiç kaydedilmiyordu; örneği yazan biz bile düştük).
// Hangi yerleşiğin saf olduğu ELLE TUTULAN bir listede değil, yerleşik tablosunun yanında
// (src/builtins.cpp: builtin_effect) — her yerleşik sınıflandırılmış olmak zorunda, sınıfsız yeni
// yerleşik CI'da kırmızı (tests/builtin_effect_table_test.sh). Kural yalnız Pure'a uygulanır:
// geri çağırma alanlar (Callback) ve tek başına "fırlatıyor mu" diye çağrılabilenler (ThrowCheck)
// DIŞARIDA; dışarıyı okuyanlar (Reads) da şimdilik dışarıda.
#include "look/builtins.h"
#include <string>

namespace look {

inline bool is_pure_builtin(const std::string& full) {
    return builtin_effect(full) == BuiltinEffect::Pure;
}

// Kullanıcıya giden açıklama (İngilizce — dil politikası). array::* için "listeyi değiştirmez"
// vurgusu, en sık düşülen tuzak o.
inline std::string unused_result_message(const std::string& full, int line) {
    std::string at = line > 0 ? "line " + std::to_string(line) + ": " : "";
    // array::pop son elemanı, array::contains bool döndürür — "yeni dizi" demek yanlış olurdu.
    const bool arr = full.rfind("array::", 0) == 0;
    const bool returns_array = arr && full != "array::pop" && full != "array::contains";
    std::string why = returns_array ? full + "() returns a new array and does not change the one you pass"
                    : arr           ? full + "() returns a value and does not change the array you pass"
                    :                 full + "() returns a value and has no other effect";
    return at + "the result of " + full + "() is not used — " + why +
           ". Assign the result, e.g. $x = " + full + "(...)";
}

} // namespace look
