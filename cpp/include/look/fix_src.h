#pragma once
// lk fix — kullanımdan kalkan yazımları yeni yazıma çevirir (gofix modeli, SEÇENEK YOK).
//
//   push(X, v)        (deyim)   → X = array::push(X, v)
//   pop(X)            (deyim)   → X = array::slice(X, 0, -1)
//   $v = pop(X)       (deyim)   → $v = X[-1]  +  yeni satırda  X = array::slice(X, 0, -1)
//   array::pop(X)     (her yer) → X[-1]
//
// X bir değişken ya da indeks/üye yolu ($l, $m["k"], $u.tags) olmalı. Başka her kullanım (daha
// büyük bir ifadenin içinde push/pop) DEĞİŞMEZ ve nedeniyle raporlanır.
// Yeniden yazma KAYNAK ARALIKLARIYLA yapılır (token konumları): dokunulmayan satırlar, yorumlar
// ve biçim olduğu gibi kalır. Sonuç yeniden ayrıştırılır; ayrışmazsa dosya değişmez.
#include <string>
#include <vector>

namespace look {

struct FixResult {
    std::string output;                 // yeniden yazılmış kaynak (hata varsa girdi)
    // Her satır "<satır>: ..." biçiminde; CLI başına dosya adını ekler.
    std::vector<std::string> changes;   // "<satır>: eski  ->  yeni"
    std::vector<std::string> notes;     // "<satır>: NOTE: ..." (boş listede davranış farkı)
    std::vector<std::string> skipped;   // "<satır>: not rewritten: neden"
    std::string error;                  // boş değilse dosya işlenemedi (çıktı = girdi)
};

FixResult look_fix_source(const std::string& src);

// lk fix CLI. files boşsa stdin → stdout. check=true → yazmaz, değişecekleri listeler.
// Dönüş: 0 temiz; 1 değişecek (check) ya da yeniden yazılamayan çağrı kaldı; 2 hata.
int run_fix(const std::vector<std::string>& files, bool check);

} // namespace look
