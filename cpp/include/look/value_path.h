#pragma once
// LOOK 2 — değer semantiği: bir indeks YOLU boyunca yazınca-kopyala ataması.
//
//   $root[k0][k1]...[kn-1] = değer
//
// Kural: dizi, map ve struct DEĞERDİR; atamak/geçmek/döndürmek alana kendi değerini verir.
// Uygulama: depolama paylaşılır ve sayılır, kopya yalnız hâlâ paylaşılan bir depoya
// YAZILIRKEN yapılır. Bunun doğru çalışması için yazma, değişkenin KENDİ yerinden başlamalı
// ve her düzeyi orada tek sahipli hale getirerek inmelidir. Diziyi önce bir geçiciye alıp
// onun üzerinden yazmak (LOOK 1'in yaptığı) geçiciyi ikinci sahip yapar: ya her yazmada kopya
// (karesel döngü) ya da kaybolan yazma olur.
//
// İki motor da bu başlığı kullanır: eleman arama (value_slot) ve yol yürüyüşü
// (value_set_path) tek yerde. Son düzeydeki yazma motorun kendi tek-düzey atamasıdır
// (VM::array_set / yorumlayıcının karşılığı) ve geri çağrı olarak verilir.
#include "look/struct_types.h"

namespace look {

// "12" / "-3" gibi TAM SAYI metni mi? Dizi anahtarı sayısal indeks mi yoksa map anahtarı mı
// diye ayırır. (Eskiden vm.cpp ve interpreter.cpp'de iki ayrı kopyaydı.)
inline bool value_is_int_key(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// Var olan elemanın YERİ; yoksa nullptr (fırlatmaz — yol ataması eksik ara düzeyi yaratır).
// Arama kuralları okuma ile aynı: map anahtarı, tam-sayı metni = sayısal indeks, negatif =
// sondan. Struct: alan adı; bilinmeyen alan ve sayısal indeks HATA (şekil sabit).
inline Value* value_slot(Value& arr, const Value& key) {
    if (arr.type() == Value::STRUCT) {
        auto& sv = *arr.vec_ptr();
        if (key.type() != Value::STRING)
            throw std::runtime_error("a struct is not an array: use a field name ('"
                                     + struct_name(sv) + "' has no numeric index)");
        int i = struct_field_index(struct_def(sv), key.str_ref());
        if (i < 0) struct_no_field(struct_name(sv), key.str_ref());
        return &sv[STRUCT_SLOT0 + (size_t)i];
    }
    if (arr.type() != Value::ARRAY) return nullptr;
    auto& vec = *arr.vec_ptr();
    if (!vec.empty() && vec[0].type() == Value::STRING && vec[0].str_ref() == "__assoc__") {
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref() : (ktmp = key.to_string());
        for (size_t i = 1; i + 1 < vec.size(); i += 2)
            if (vec[i].type() == Value::STRING ? vec[i].str_ref() == k : vec[i].to_string() == k)
                return &vec[i + 1];
        return nullptr;
    }
    if (key.type() == Value::STRING && !value_is_int_key(key.str_ref())) return nullptr;
    int64_t i = key.to_int();
    if (i < 0) i += (int64_t)vec.size();
    if (i < 0 || i >= (int64_t)vec.size()) return nullptr;
    return &vec[(size_t)i];
}

// slot[k0][k1]...[kn-1] üzerinde son düzeyi `set_one(container, key, nullptr)` ile yazar.
//   * her düzey önce detach edilir (paylaşılıyorsa üst düzeyi kopyalanır);
//   * eksik ya da null ara düzey yaratılır (sonraki anahtar map anahtarıysa map, değilse liste);
//   * struct alanı tipli olduğu için null bir alanın İÇİNE yazılmaz — hata.
template <class SetOne>
inline void value_set_path(Value& slot, const Value* keys, int n, SetOne&& set_one) {
    slot.detach();
    if (n == 1) { set_one(slot, keys[0], nullptr); return; }
    Value* child = value_slot(slot, keys[0]);
    if (slot.type() == Value::STRUCT) {
        if (child->type() == Value::NONE)
            throw std::runtime_error("struct '" + struct_name(*slot.vec_ptr()) + "': field '"
                                     + keys[0].to_string() + "' is null; assign a value to it first");
    } else if (!child || child->type() == Value::NONE) {
        const bool map_key = keys[1].type() == Value::STRING && !value_is_int_key(keys[1].str_ref());
        auto fresh = std::make_shared<std::vector<Value>>();
        if (map_key) fresh->push_back(Value(std::string("__assoc__")));
        Value holder(fresh);
        // Ara düzeyi motorun kendi tek-düzey atamasıyla yerleştir (dönüşüm kuralları onda).
        // (forced != nullptr: bu değeri olduğu gibi yerleştir; nullptr: asıl yazma)
        set_one(slot, keys[0], &holder);
        child = value_slot(slot, keys[0]);
        if (!child) throw std::runtime_error("cannot assign through index " + keys[0].to_string());
    }
    value_set_path(*child, keys + 1, n - 1, set_one);
}


// slot[k0]...[kn-1] konumundaki değerin YERİNİ verir; yol boyunca ve hedefin kendisinde
// yazınca-kopyala uygular (n == 0: kökün kendisi). push/pop gibi "değişkeni yerinde
// değiştiren" işlemler için: hedef tek sahipli olarak döner, çağıran doğrudan değiştirir.
template <class SetOne>
inline Value& value_path_slot(Value& slot, const Value* keys, int n, SetOne&& set_one) {
    Value* cur = &slot;
    for (int i = 0; i < n; ++i) {
        cur->detach();
        Value* child = value_slot(*cur, keys[i]);
        if (cur->type() == Value::STRUCT) {
            // struct alanı: olduğu gibi kullanılır (null ise çağıran "dizi değil" der)
        } else if (!child) {
            Value holder(std::make_shared<std::vector<Value>>());
            set_one(*cur, keys[i], &holder);
            child = value_slot(*cur, keys[i]);
            if (!child) throw std::runtime_error("cannot reach index " + keys[i].to_string());
        }
        cur = child;
    }
    cur->detach();
    return *cur;
}

// push/pop'un tek tanımı (iki motor ortak). Hedef tek sahipli bir yerdir.
inline Value value_push(Value& target, const Value& v) {
    if (target.type() != Value::ARRAY) throw std::runtime_error("push() requires array as first argument");
    target.vec_ptr()->push_back(v);
    // Yeni uzunluk döner. (LOOK 1 dizinin kendisini döndürüyordu; değer semantiğinde o,
    // değişkenin deposuna ikinci bir sahip olur ve bir sonraki push'u kopyaya zorlardı.)
    return Value((int64_t)target.vec_ptr()->size());
}
inline Value value_pop(Value& target) {
    if (target.type() != Value::ARRAY) throw std::runtime_error("pop() requires array");
    auto& vec = *target.vec_ptr();
    if (vec.empty()) return Value();
    Value last = std::move(vec.back());
    vec.pop_back();
    return last;
}
} // namespace look
