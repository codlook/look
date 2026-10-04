#pragma once
// LOOK 2 — tipli struct: ad + TİP + varsayılan; struct kendi DEĞER TÜRÜDÜR (Value::STRUCT).
//
//   struct User {
//       name  string              # varsayılansız → sıfır değeri ("")
//       age   int = 18            # tipli + varsayılan
//       score ?float              # null olabilir
//       boss  User                # başka bir struct (null olabilir)
//       note  any                 # her değeri alır
//   }
// Tek yazım: `ad tip [= varsayılan]`. Tipsiz alan ve v1'in `ad: varsayılan` yazımı
// ayrıştırıcıda reddedilir (parser.cpp).
//
// DÜZEN (iki motorda aynı, tek yerde tanımlı):
//   tanım (def)  = ARRAY, üçlüler [alan, varsayılan, tip]          (bildirim sırası)
//   örnek        = Value::STRUCT, slotlar [ad, def, alan0, alan1, ...]
// Alan i'nin değeri slot 2+i'dedir → erişim ad araması değil İNDEKSTİR. Struct bir dizi
// değildir: array:: fonksiyonları, sayısal indeksleme ve dizi sanan her kod onu reddeder
// (v1'de struct içeride bir assoc dizisiydi; array::set tip denetimini atlatabiliyordu).
//
// Kurallar HER İKİ motorda bu tek başlıktan gelir (iki ayrı uygulama = ayrışma):
//   * tip adı bilinmeli (yerleşik ya da bildirilmiş bir struct);
//   * alana uymayan değer → hata (kurulumda da, sonradan atamada da);
//   * int, float alana yazılabilir (float'a çevrilir); tersi ve diğer tüm karışımlar hata;
//   * null yalnız ?tip, any, fn ve struct tipli alanlara yazılabilir;
//   * şekil SABİT: bildirilmemiş alan yok (ne okunur ne yazılır).
#include "look/interpreter.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace look {

constexpr size_t STRUCT_SLOT0 = 2;   // slot vektöründe ilk alanın konumu

inline const std::string& struct_name(const std::vector<Value>& sv)       { return sv[0].str_ref(); }
inline const std::vector<Value>& struct_def(const std::vector<Value>& sv) { return *sv[1].as_array(); }
inline size_t struct_field_count(const std::vector<Value>& def)           { return def.size() / 3; }

// Kullanıcıya gösterilen tip adı (hata mesajları).
inline std::string struct_value_type_name(const Value& v) {
    switch (v.type()) {
        case Value::NONE:   return "null";
        case Value::INT:    return "int";
        case Value::FLOAT:  return "float";
        case Value::STRING: return "string";
        case Value::BOOL:   return "bool";
        case Value::FUNCTION: case Value::BYTECODE_FN: return "fn";
        case Value::STRUCT: return struct_name(*v.as_struct());
        case Value::ARRAY: {
            const auto& a = *v.as_array();
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
// gelir. Struct'lar birbirine ileriden başvurabildiği için denetim bildirimde değil, struct
// kurulurken (ve web'de kurulumun sonunda toplu olarak) yapılır.
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
    if (def.type() == Value::ARRAY || def.type() == Value::STRUCT) return def.deep_clone();
    return def;
}

// Değer alan tipine uyuyor mu? Uymuyorsa fırlatır; int→float'ı yerinde çevirir.
inline void struct_check_field(const std::string& sname, const std::string& fname,
                               const std::string& type, Value& v) {
    if (type.empty()) return;
    // Hızlı yol (her alan yazımında çalışır): yaygın eşleşmeler metin üretmeden biter.
    switch (v.type()) {
        case Value::INT:    if (type == "int"    || type == "?int")    return; break;
        case Value::STRING: if (type == "string" || type == "?string") return; break;
        case Value::FLOAT:  if (type == "float"  || type == "?float")  return; break;
        case Value::BOOL:   if (type == "bool"   || type == "?bool")   return; break;
        default: break;
    }
    if (type == "any" || type == "?any") return;
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

// Alan adı → alan sırası (0..n-1); yoksa -1. Tanım üzerinde doğrusal arama: yalnız önbellek
// ıskasında ve yorumlayıcıda çalışır; VM sıcak yolu sonucu talimat başına önbellekler.
inline int struct_field_index(const std::vector<Value>& def, const std::string& field) {
    for (size_t j = 0; j + 2 < def.size(); j += 3)
        if (def[j].str_ref() == field) return (int)(j / 3);
    return -1;
}

// Yeni örnek: her alan varsayılanıyla (örnek başına kopya), tip adları ve varsayılanlar
// denetlenmiş. Literal alanları ardından struct_set ile yerine konur.
template <class IsStruct>
inline Value struct_new(const std::string& sname, const Value& def_value, IsStruct&& is_struct) {
    const auto& def = *def_value.as_array();
    auto sv = std::make_shared<std::vector<Value>>();
    sv->reserve(STRUCT_SLOT0 + struct_field_count(def));
    sv->push_back(Value(sname));
    sv->push_back(def_value);
    for (size_t j = 0; j + 2 < def.size(); j += 3) {
        const std::string& fname = def[j].str_ref();
        const std::string  ftype = def[j + 2].to_string();
        struct_check_type_known(sname, fname, ftype, is_struct);
        Value dv = struct_instance_default(def[j + 1], ftype);
        struct_check_field(sname, fname, ftype, dv);
        sv->push_back(std::move(dv));
    }
    return Value::make_struct(std::move(sv));
}

// i. alana tip denetimli yazma.
inline void struct_set(std::vector<Value>& sv, int i, Value v) {
    const auto& def = struct_def(sv);
    // str_ref(): tip adı kopyalanmaz (to_string her yazmada bir std::string ayırıyordu).
    struct_check_field(struct_name(sv), def[(size_t)i * 3].str_ref(), def[(size_t)i * 3 + 2].str_ref(), v);
    sv[STRUCT_SLOT0 + (size_t)i] = std::move(v);
}

// Ada göre okuma/yazma (yorumlayıcı ve VM'in önbelleksiz yolu). Bilinmeyen alan → hata.
inline const Value& struct_get_named(const std::vector<Value>& sv, const std::string& field) {
    int i = struct_field_index(struct_def(sv), field);
    if (i < 0) struct_no_field(struct_name(sv), field);
    return sv[STRUCT_SLOT0 + (size_t)i];
}
inline void struct_set_named(std::vector<Value>& sv, const std::string& field, Value v) {
    int i = struct_field_index(struct_def(sv), field);
    if (i < 0) struct_no_field(struct_name(sv), field);
    struct_set(sv, i, std::move(v));
}

// Struct'ın alanlarını map olarak verir (["__assoc__", ad, değer, ...]) — JSON, şablon ve
// foreach gibi SICAK OLMAYAN tüketiciler için. Değerler paylaşılır (kopya değil).
inline Value struct_to_map(const Value& s) {
    const auto& sv = *s.as_struct();
    const auto& def = struct_def(sv);
    auto m = std::make_shared<std::vector<Value>>();
    m->reserve(1 + 2 * struct_field_count(def));
    m->push_back(Value(std::string("__assoc__")));
    for (size_t j = 0, i = 0; j + 2 < def.size(); j += 3, ++i) {
        m->push_back(def[j]);
        m->push_back(sv[STRUCT_SLOT0 + i]);
    }
    return Value(m);
}

} // namespace look
