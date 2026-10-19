#pragma once
// Sonucu kullanılmayan saf yerleşik çağrısı: `array::push($list, $x)` tek başına bir cümle.
// array::push listeyi YERİNDE değiştirmez, yeni liste döndürür — sonuç atılınca hiçbir şey olmaz,
// hata da çıkmaz (VS Code örneğinde not hiç kaydedilmiyordu; örneği yazan biz bile düştük).
// v1: derleyici UYARI toplar (CompiledProgram::warnings), program aynen çalışır; LOOK 2'de
// program başlamaz.
// Liste muhafazakârdır — yalnız şu üç koşulu birden sağlayanlar:
//   * argümanlarını değiştirmez, dışarıya hiçbir etkisi yoktur (dosya, ağ, önbellek, çıktı yok)
//   * geri çağırma (callback) almaz — array::map/filter/reduce/sort/find/any/all DIŞARIDA: geri
//     çağırmanın kendi yan etkisi olabilir
//   * yalnız hata fırlatsın diye çağrılmaz — json::decode, date::parse, *_decode, rs256_verify
//     DIŞARIDA: tek başına çağırıp geçerlilik denetlemek meşru bir kullanım
#include <set>
#include <string>

namespace look {

inline bool is_pure_builtin(const std::string& full) {
    static const std::set<std::string> pure = {
        // array — geri çağırmasızlar
        "array::push", "array::pop", "array::set", "array::slice", "array::reverse", "array::unique",
        "array::flatten", "array::chunk", "array::keys", "array::values", "array::zip",
        "array::contains", "array::new_assoc",
        // string
        "string::contains", "string::ends_with", "string::starts_with", "string::format",
        "string::index_of", "string::join", "string::len", "string::lower", "string::upper",
        "string::ltrim", "string::rtrim", "string::trim", "string::pad_left", "string::pad_right",
        "string::repeat", "string::replace", "string::reverse", "string::slugify", "string::split",
        "string::substr",
        // math
        "math::abs", "math::ceil", "math::floor", "math::round", "math::max", "math::min",
        "math::pow", "math::sqrt", "math::sin", "math::cos", "math::tan", "math::log", "math::pi",
        // type
        "type::is_array", "type::is_bool", "type::is_float", "type::is_function", "type::is_int",
        "type::is_null", "type::is_string", "type::of", "type::to_bool", "type::to_float",
        "type::to_int", "type::to_string",
        // html, json, crypto (kodlama/özet), date (biçim/hesap)
        "html::escape", "html::attr", "html::strip",
        "json::encode",
        "crypto::sha256", "crypto::hmac_sha256", "crypto::hmac_sha256_raw", "crypto::base64_encode",
        "crypto::base64url_encode", "crypto::hex_encode", "crypto::constant_compare",
        "date::format", "date::add", "date::sub", "date::diff", "date::from_timestamp",
        "date::weekday", "date::week",
    };
    return pure.count(full) != 0;
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
