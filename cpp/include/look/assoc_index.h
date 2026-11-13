#pragma once
// Sözlük anahtar araması — büyük sözlükte O(1).
//
// Sözlük depolaması değişmez: ["<işaret>", k0, v0, k1, v1, …] (sıra = ekleme sırası; foreach,
// json, array:: hepsi bu düzeni okur). ESKİ: her okuma ve her yazma listeyi baştan tarıyordu —
// 40 000 anahtarlı gövdeyi döngüde kopyalamak 4,3 sn (karesel); tek istek süreci meşgul ediyordu.
//
// Dizin: anahtar KONUMLARINDAN oluşan açık adresli tablo (uint32; anahtar metni kopyalanmaz).
// Hash süreç başına rastgele tohumlu SipHash-1-3 (Rust/Python'daki gibi): tohum bilinmeden aynı
// hücreye düşen anahtarlar üretilemez (hash flooding).
//
// Geçerlilik: dizin, kurulduğu andaki depo boyunu (seen) bilir.
//   * Okuma yolu dizini YALNIZ boştan kurar (CAS) ve hiç değiştirmez; boy farklıysa doğrusal tarar.
//   * Yazma yolu (assoc_sync) depoyu tek sahipli tutar: uca eklenenleri dizine katar, boy
//     küçüldüyse yeniden kurar.
//   * Her isabet anahtar metniyle doğrulanır; tutmazsa (sıralama gibi ortadan değişiklik) yazma
//     yolunda yeniden kurulur, okuma yolunda doğrusal taranır.
// Ortadan anahtar silen/değiştiren ve boyu koruyan bir yol, dizini bırakmalıdır (drop_index).
// LOOK_ASSOC_INDEX_VERIFY ile derlenince her dizinli arama doğrusal taramayla karşılaştırılır
// (tests/assoc_index_test.sh tüm testleri bu kipte koşar).
#include "look/value.h"
#include <cstdlib>
#include <cstdio>

namespace look {

uint64_t look_key_hash(const char* p, size_t n) noexcept;   // value.cpp (SipHash-1-3, süreç tohumu)

struct AssocIndex {
    std::vector<uint32_t> slots;   // 0 = boş; diğerleri anahtarın depodaki konumu (tek sayı ≥ 1)
    size_t  seen = 0;              // dizinin kapsadığı depo boyu
    size_t  next = 1;              // dizine katılacak sıradaki anahtar konumu
    size_t  count = 0;
    bool    unusable = false;      // metin olmayan anahtar / eski __struct__ etiketi → doğrusal
};

// Dizin bu boydan küçük sözlükte kurulmaz: birkaç metin karşılaştırması tablodan hızlıdır.
constexpr size_t LOOK_ASSOC_INDEX_MIN_PAIRS = 8;

namespace assoc_detail {

inline ValueArray& va(const std::vector<Value>& v) {
    // Her dizi/sözlük depolaması ValueArray'dir (look_new_array / make_shared<ValueArray>;
    // tests/value_release_test.sh düz std::vector<Value> kuran satırı reddeder).
    return static_cast<ValueArray&>(const_cast<std::vector<Value>&>(v));
}

// Okuma eski "__struct__" etiketini atlar (start 3); yazma her zaman 1'den tarar (değişmeyen davranış).
inline size_t linear(const std::vector<Value>& vec, const std::string& k, bool for_write = false) {
    size_t start = 1;
    if (!for_write && vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__") start = 3;
    for (size_t i = start; i + 1 < vec.size(); i += 2)
        if (vec[i].type() == Value::STRING ? vec[i].str_ref() == k : vec[i].to_string() == k) return i;
    return 0;
}

inline void place(AssocIndex& ix, const std::vector<Value>& vec, size_t pos) {
    const std::string& k = vec[pos].str_ref();
    const size_t mask = ix.slots.size() - 1;
    for (size_t h = (size_t)look_key_hash(k.data(), k.size()) & mask;; h = (h + 1) & mask) {
        uint32_t s = ix.slots[h];
        if (s == 0) { ix.slots[h] = (uint32_t)pos; ++ix.count; return; }
        if (vec[s].str_ref() == k) return;   // yinelenen anahtar: ilk geçen kazanır (doğrusal tarama gibi)
    }
}

inline void grow(AssocIndex& ix, const std::vector<Value>& vec, size_t want_pairs) {
    size_t cap = 16;
    while (cap < want_pairs * 2) cap <<= 1;
    if (cap <= ix.slots.size()) return;
    std::vector<uint32_t> old;
    old.swap(ix.slots);
    ix.slots.assign(cap, 0);
    ix.count = 0;
    for (uint32_t s : old) if (s) place(ix, vec, s);
}

// Uca eklenen çiftleri dizine kat (seen/next ilerler). Metin olmayan anahtar → kullanılmaz.
inline void extend(AssocIndex& ix, const std::vector<Value>& vec) {
    size_t i = ix.next;
    if (vec.size() > ix.seen) grow(ix, vec, ix.count + (vec.size() - ix.seen) / 2 + 1);
    for (; i + 1 < vec.size(); i += 2) {
        if (vec[i].type() != Value::STRING || vec[i].is_assoc_marker()) { ix.unusable = true; break; }
        place(ix, vec, i);
    }
    ix.next = i;
    ix.seen = vec.size();
}

inline AssocIndex* build(const std::vector<Value>& vec) {
    auto* ix = new AssocIndex();
    if (vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__") {
        ix->unusable = true; ix->seen = vec.size();
        return ix;
    }
    extend(*ix, vec);
    return ix;
}

inline size_t probe(const AssocIndex& ix, const std::vector<Value>& vec, const std::string& k) {
    const size_t mask = ix.slots.size() - 1;
    for (size_t h = (size_t)look_key_hash(k.data(), k.size()) & mask;; h = (h + 1) & mask) {
        uint32_t s = ix.slots[h];
        if (s == 0) return 0;
        if (s < vec.size() && vec[s].type() == Value::STRING && vec[s].str_ref() == k) return s;
        if (s >= vec.size() || vec[s].type() != Value::STRING) return SIZE_MAX;   // bayat
        // aynı hücreye düşen başka anahtar: sonraki hücre
    }
}

#ifdef LOOK_ASSOC_INDEX_VERIFY
inline size_t verified(const std::vector<Value>& vec, const std::string& k, size_t got) {
    size_t want = linear(vec, k);
    if (got != want) {
        std::fprintf(stderr, "ASSOC INDEX MISMATCH: key [%s] index %zu linear %zu size %zu\n",
                     k.c_str(), got, want, vec.size());
        std::abort();
    }
    return got;
}
#else
inline size_t verified(const std::vector<Value>&, const std::string&, size_t got) { return got; }
#endif

} // namespace assoc_detail

// Sözlükte k anahtarının KONUMU (değer konum+1'de); yoksa 0. vec bir sözlük olmalı (vec[0] işaret).
// Depoyu değiştirmez (okuma yolu; paylaşılan sözlükte birden çok iş parçacığı çağırabilir).
inline size_t assoc_find(const std::vector<Value>& vec, const std::string& k) {
    using namespace assoc_detail;
    if (vec.size() < 1 + 2 * LOOK_ASSOC_INDEX_MIN_PAIRS) return linear(vec, k);
    ValueArray& a = va(vec);
    AssocIndex* ix = a.assoc_idx.load(std::memory_order_acquire);
    if (!ix) {
        AssocIndex* fresh = build(vec);
        AssocIndex* expected = nullptr;
        if (a.assoc_idx.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            ix = fresh;
        else { look_assoc_index_free(fresh); ix = expected; }
    }
    if (ix->unusable || ix->seen != vec.size()) return linear(vec, k);
    size_t p = probe(*ix, vec, k);
    if (p == SIZE_MAX) return linear(vec, k);
    return verified(vec, k, p);
}

// Yazma yolu: depo tek sahipli (yazınca-kopyala). Dizini depoyla eşitler, sonra arar.
inline size_t assoc_find_for_write(std::vector<Value>& vec, const std::string& k) {
    using namespace assoc_detail;
    if (vec.size() < 1 + 2 * LOOK_ASSOC_INDEX_MIN_PAIRS) return linear(vec, k, true);
    ValueArray& a = va(vec);
    AssocIndex* ix = a.assoc_idx.load(std::memory_order_relaxed);
    if (ix && (vec.size() < ix->seen || ix->next > vec.size())) { a.drop_index(); ix = nullptr; }
    if (!ix) { ix = build(vec); a.assoc_idx.store(ix, std::memory_order_release); }
    else if (vec.size() != ix->seen && !ix->unusable) extend(*ix, vec);
    if (ix->unusable) return linear(vec, k, true);
    size_t p = probe(*ix, vec, k);
    if (p == SIZE_MAX) {   // ortadan değişmiş: yeniden kur
        a.drop_index();
        ix = build(vec); a.assoc_idx.store(ix, std::memory_order_release);
        if (ix->unusable) return linear(vec, k, true);
        p = probe(*ix, vec, k);
        if (p == SIZE_MAX) return linear(vec, k, true);
    }
    return verified(vec, k, p);   // dizin kullanılabiliyorsa __struct__ etiketi yok → iki tarama aynı
}

// Yazma yolu: assoc_find_for_write 0 döndükten sonra çağıran k'yi ve değeri uca ekledi.
inline void assoc_appended(std::vector<Value>& vec) {
    AssocIndex* ix = assoc_detail::va(vec).assoc_idx.load(std::memory_order_relaxed);
    if (ix && !ix->unusable && ix->seen + 2 == vec.size()) assoc_detail::extend(*ix, vec);
}

} // namespace look
