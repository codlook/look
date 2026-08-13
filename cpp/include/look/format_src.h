#pragma once
// lk fmt — LOOK kaynak biçimlendirici (gofmt modeli: SEÇENEK YOK, tek kanonik biçim).
//
// V1 = yeniden-girinti: girinti = süslü-parantez/köşeli/parantez derinliği × 4; sondaki
// boşluk kırpılır; ardışık boş satırlar tek boşluğa indirilir. Token İÇERİĞİNE dokunmaz
// (yalnız baştaki/sondaki boşluk) → anlam inşaen korunur. Yorumlar korunur (char-tabanlı).
//
// GÜVENCE: biçimlenmiş çıktı yeniden lex'lenip trivia-dışı token dizisi girdiyle
// karşılaştırılır; UYMAZSA biçimleme İPTAL edilir (dosya değişmez). Bu, edge-case'lerde
// (çok-satırlı string vb.) bozma yerine güvenli-geri-çekilme garantisi verir.
#include <string>
#include <vector>

namespace look {

// Kaynağı kanonik biçime getir. Anlam-değiştirmezse formatted döner; string tracking bir
// edge-case'e takılırsa (verify başarısız) orijinali AYNEN döndürür (ok=false ile bildirir).
std::string look_format_source(const std::string& src, bool* ok = nullptr);

// lk fmt CLI. files boşsa stdin. check=true → CI modu: biçimsiz dosya varsa exit 1 (yazMAZ).
// Aksi halde dosyaları yerinde biçimler. Dönüş: 0 temiz, 1 check'te biçimsiz, 2 hata.
int run_fmt(const std::vector<std::string>& files, bool check);

} // namespace look
