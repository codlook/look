#pragma once
// LOOK 2 — tipli struct alanları: ad + TİP + varsayılan.
//
//   struct User {
//       name  string              # tipli, varsayılansız → sıfır değeri ("")
//       age   int = 18            # tipli + varsayılan
//       score ?float              # null olabilir
//       boss  User                # başka bir struct (null olabilir)
//       note  any                 # her değeri alır
//   }
// Tek yazım: `ad tip [= varsayılan]`. Tipsiz alan ve v1'in `ad: varsayılan` yazımı
// ayrıştırıcıda reddedilir (parser.cpp).
//
// Kurallar HER İKİ motorda bu tek başlıktan gelir (iki ayrı uygulama = ayrışma):
//   * tip adı bilinmeli (yerleşik ya da bildirilmiş bir struct) — yazım hatası (`strng`)
//     struct ilk kurulduğunda, alana değer verilmese bile hata;
//   * alana uymayan değer → hata (kurulumda da, sonradan atamada da);
//   * int, float alana yazılabilir (float'a çevrilir); tersi ve diğer tüm karışımlar hata;
//   * null yalnız ?tip, any, fn ve struct tipli alanlara yazılabilir;
//   * struct'ın şekli SABİT: bildirilmemiş alana atama hata (v1'de sessizce ekleniyordu).
// Bildirim düzeni (VM'de gizli global "__sdef:Name"): üçlüler [ad, varsayılan, tip].
#include "look/interpreter.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace look {

inline bool struct_is_instance(const std::vector<Value>& v) {
    return v.size() > 2 && v[0].type() == Value::STRING && v[0].str_ref() == "__assoc__"
        && v[1].type() == Value::STRING && v[1].str_ref() == "__struct__";
}

// Kullanıcıya gösterilen tip adı (hata mesajları).
inline std::string struct_value_type_name(const Value& v) {
    switch (v.type()) {
        case Value::NONE:   return "null";
        case Value::INT:    return "int";
        case Value::FLOAT:  return "float";
        case Value::STRING: return "string";
        case Value::BOOL:   return "bool";
        case Value::FUNCTION: case Value::BYTECODE_FN: return "fn";
        case Value::ARRAY: {
            const auto& a = *v.as_array();
            if (struct_is_instance(a)) return a[2].to_string();
            if (!a.empty() && a[0].type() == Value::STRING && a[0].str_ref() == "__assoc__") return "map";
            return "array";
        }
        default: return "value";
    }
}

inline bool struct_type_is_builtin(const std::string& base) {
    return base == "int" || base == "float" || base == "string" || base == "bool"
        || base == "array" || base == "map" || base == "fn" || base == "any";
}

// Tip adı bilinen bir şey mi: yerleşik tip ya da bildirilmiş struct. `is_struct(ad)` motordan
// gelir (VM: "__sdef:ad" global'i; tree-walk: struct_defs_). Struct'lar birbirine ileriden
// başvurabildiği için denetim bildirimde değil, struct İLK KURULDUĞUNDA yapılır.
template <class IsStruct>
inline void struct_check_type_known(const std::string& sname, const std::string& fname,
                                    const std::string& type, IsStruct&& is_struct) {
    if (type.empty()) return;
    const std::string base = type[0] == '?' ? type.substr(1) : type;
    if (struct_type_is_builtin(base) || is_struct(base)) return;
    throw std::runtime_error("struct '" + sname + "': field '" + fname + "' has unknown type '"
                             + base + "' (expected int, float, string, bool, array, map, fn, any "
                               "or the name of a struct)");
}

// Tipli alanın varsayılansız değeri. array/map HER örnek için taze üretilir.
inline Value struct_zero_value(const std::string& type) {
    if (type.empty() || type[0] == '?') return Value();
    if (type == "int")    return Value((int64_t)0);
    if (type == "float")  return Value(0.0);
    if (type == "string") return Value(std::string());
    if (type == "bool")   return Value(false);
    if (type == "array")  return Value(std::make_shared<std::vector<Value>>());
    if (type == "map") {
        auto m = std::make_shared<std::vector<Value>>();
        m->push_back(Value(std::string("__assoc__")));
        return Value(m);
    }
    return Value();   // any, fn, struct adı → null
}

// Varsayılan değeri örneğe koyarken: dizi/map/struct DERİN kopyalanır. Eskiden aynı dizi
// tüm örnekler arasında paylaşılıyordu; web'de bildirim istekten uzun yaşadığı için bir
// isteğin verisi başka kullanıcının yeni örneğinde görünüyordu (1.0.3 güvenlik düzeltmesi).
// Üst düzey kopya YETMEZ: iç içe dizi yine paylaşılır.
inline Value struct_instance_default(const Value& def, const std::string& type) {
    if (def.type() == Value::NONE) return struct_zero_value(type);
    if (def.type() == Value::ARRAY) return def.deep_clone();
    return def;
}

// Değer alan tipine uyuyor mu? Uymuyorsa fırlatır; int→float'ı yerinde çevirir.
inline void struct_check_field(const std::string& sname, const std::string& fname,
                               const std::string& type, Value& v) {
    if (type.empty()) return;
    const bool nullable = type[0] == '?';
    const std::string base = nullable ? type.substr(1) : type;
    if (base == "any") return;
    if (v.type() == Value::NONE) {
        if (nullable || base == "fn" || !struct_type_is_builtin(base)) return;
        throw std::runtime_error("struct '" + sname + "': field '" + fname + "' expects "
                                 + base + ", got null (declare it as ?" + base + " to allow null)");
    }
    if (base == "float" && v.type() == Value::INT) { v = Value((double)v.as_int()); return; }
    std::string got = struct_value_type_name(v);
    if (got == base) return;
    // Boş liste [] henüz anahtar almamış bir map olarak da yazılabilir.
    if (base == "map" && got == "array" && v.as_array()->empty()) return;
    throw std::runtime_error("struct '" + sname + "': field '" + fname + "' expects "
                             + type + ", got " + got);
}

[[noreturn]] inline void struct_no_field(const std::string& sname, const std::string& fname) {
    throw std::runtime_error("struct '" + sname + "' has no field '" + fname + "'");
}

} // namespace look
