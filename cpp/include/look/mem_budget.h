#pragma once
// Bellek bütçesi — bir istek bütün süreci öldüremez (Go GOMEMLIMIT modeli, tek ayar).
//
// ESKİ: istek başına ve süreç geneli hiçbir bellek sınırı yoktu. Sınırsız büyüyen TEK bir istek
// (döngüde büyüyen liste/metin) 512 MB'lık kapta sunucuyu 1 sn'den kısa sürede öldürüyordu
// (çekirdeğin bellek katili, rc=137 — ölçüldü); o andan sonra hiçbir istek yanıt almıyordu.
//
// Model:
//   * Süreç sınırı: LOOK_MEMORY_LIMIT (Go'nun GOMEMLIMIT biçimi: 512MiB, 1GiB, bayt). Verilmezse
//     otomatik: kabın (cgroup memory.max) %80'i, kap sınırı yoksa 1 GiB. Ayar listesinde tek bellek
//     ayarı budur; istek bütçesi ayrı ayar DEĞİLDİR.
//   * İstek bütçesi: otomatik — 256 MiB, süreç sınırının yarısını geçmez.
//   * Sayım: C++'ın her ayırması (operator new/delete) o anki isteğin hesabına ve süreç sayacına
//     yazılır. İstek hesabı iş parçacığı yereldir, fiber geçişinde fiber'ın hesabına çevrilir;
//     serbest bırakma hesabı sıfırın altına indirmez (başka isteğin ayırdığı paylaşılan veri).
//     Süreç sayacına iş parçacığı başına 1 MiB'lık partilerle yazılır (atomik çekişme yok).
//   * Hata ayırmanın ORTASINDA değil, VM'in güvenli noktalarında (döngü dönüşü, çağrı) verilir:
//     C++ kodu yarıda kesilmez. Sınırı aşan ayırma sıra bırakma sayacını sıfırlar → bir sonraki
//     güvenli nokta hemen denetler. Büyük tek ayırma yapan yerleşikler önceden mem_reserve() ile sorar.
//   * Süreç sınırının %90'ı dolunca yeni istekler 503 alır (yarıdakiler sürer); %100'de EN ÇOK bellek
//     kullanan istek açık hatayla biter (Linux OOM gibi — o an ayırma yapan küçük istek değil).
//   * Bellek durdurması LOOK try/catch'iyle YAKALANAMAZ (Go/Rust'ta bellek tükenmesi de yakalanmaz):
//     yakalanabilseydi `try { … } catch` döngüsü durdurmayı yutup küçük ayırmalarla büyümeyi sürdürürdü.
//   * Yanıt: kendi bütçesini aşan istek 500 (uygulamanın hatası); sunucu dolu olduğu için durdurulan en
//     büyük istek 503 + Retry-After (istemci yeniden deneyebilir) — %90'da reddedilenle aynı kod.
//   * Komut satırı (lk): varsayılan sınır YOK (Go gibi — betiği çalıştıran ne yaptığını bilir). Yalnız
//     LOOK_MEMORY_LIMIT açıkça verilirse süreç sınırı uygulanır; istek bütçesi hiç uygulanmaz.
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace look {

// Bellek durdurması — VM try/catch köprüsü bunu LOOK koduna teslim etmez (exit gibi geçer).
// server_full: sunucu dolu olduğu için durduruldu (503); false: kendi istek bütçesini aştı (500).
class MemoryLimitError : public std::runtime_error {
public:
    MemoryLimitError(const std::string& m, bool full) : std::runtime_error(m), server_full(full) {}
    bool server_full;
};

struct MemAccount {
    std::atomic<int64_t> bytes{0};      // tek yazar: hesabın isteği (iş parçacığı / fiber)
    int64_t              limit = 0;     // istek bütçesi
    std::atomic<bool>    kill{false};   // süreç sınırı: en büyük istek seçildi
    MemAccount*          next = nullptr;   // etkin hesaplar listesi (kilit altında)
    MemAccount*          prev = nullptr;
};

// Süreç başında bir kez (web kipleri): sınırı belirler ve bütçeyi açar. Geçersiz LOOK_MEMORY_LIMIT →
// false + why (süreç nedenini yazıp çıkar). describe: günlük satırı ("memory limit 1 GiB (no …)").
bool mem_budget_configure(std::string& why, std::string& describe);
// Komut satırı (lk): yalnız LOOK_MEMORY_LIMIT verildiyse süreç sınırı (istek bütçesi yok, otomatik
// değer yok). Geçersiz değer → false + why.
bool mem_budget_configure_cli(std::string& why);
bool mem_budget_enabled();
int64_t mem_process_limit();      // bayt
int64_t mem_request_limit();      // bayt
int64_t mem_process_bytes();      // sayılan canlı bayt (yaklaşık: ≤ iş parçacığı başına 1 MiB fark)

// Yeni istek kabul edilebilir mi (%90 altı). Hayırsa çağıran 503 döner.
bool mem_admit();

// İstek kapsamı: hesabı kaydeder, iş parçacığına (fiber içindeyse fiber'a da) bağlar; yıkıcı geri alır.
class MemScope {
public:
    MemScope();
    ~MemScope();
    MemScope(const MemScope&) = delete;
    MemScope& operator=(const MemScope&) = delete;
    MemAccount& account() { return acct_; }
private:
    MemAccount  acct_;
    MemAccount* prev_ = nullptr;
};

// Güvenli nokta denetimi (VM): istek bütçesi aşıldı ya da süreç sınırında bu istek seçildiyse
// LookVmError fırlatır. Hızlı yol çağıranda (fiber.h preempt sayacı) — bu yalnız yavaş yolda çağrılır.
void mem_check();
// Büyük tek ayırmadan ÖNCE (string::repeat, file::read, json::decode, range …): bytes eklenince istek
// bütçesi ya da süreç sınırı aşılacaksa hata (ayırma hiç yapılmaz). what: "string::repeat()".
void mem_reserve(int64_t bytes, const char* what);

// Fiber geçişi (fiber_posix.cpp / fiber_win.cpp): iş parçacığının o anki hesabı.
MemAccount* mem_current_account() noexcept;
void        mem_set_current_account(MemAccount* a) noexcept;

std::string mem_format_bytes(int64_t b);   // "256 MiB"

} // namespace look
