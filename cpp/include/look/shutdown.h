#pragma once
// Zarif kapanma (lk-fcgi: HTTP kipi, FastCGI TCP ve pipe kipi).
//
// ESKİ: SIGTERM/SIGINT için işleyici yoktu — systemd stop/restart (Plesk sürüm geçişi, deploy) süreci
// anında öldürüyordu: yarıdaki istekler kesiliyor, istemci yarım yanıt ya da bağlantı hatası
// görüyordu, veritabanı bağlantıları kapatılmadan bırakılıyordu.
//
// Model (Go http.Server.Shutdown): ilk sinyalde yeni bağlantı kabulü durur, boşta bekleyen
// bağlantılar kapanır, işlenen istekler biter (yanıtları "Connection: close" ile), veritabanı
// havuzları kapatılır, süreç 0 ile çıkar. Üst sınır sabit 30 sn (ayar yok — tek seçenek); bitmeyen
// istek kalırsa nedeni yazılıp 1 ile çıkılır. İkinci sinyal hemen çıkarır.
//
// Sinyaller ayrı bir iş parçacığında sigwait ile alınır: install() süreçte HİÇBİR iş parçacığı
// açılmadan çağrılmalıdır (engelleme maskesi iş parçacıklarına kalıtılır; aksi halde sinyal
// veritabanı iş parçacığı gibi rastgele birine düşer ve varsayılan eylem süreci öldürür).
#include <functional>

namespace look::shutdown {

// Süreç başında (main'in ilk işi). Windows: hiçbir şey yapmaz (varsayılan davranış sürer).
void install();

// Kapanma başlayınca çağrılacak iş (sunucu kurulduktan sonra kaydedilir). Kayıttan ÖNCE gelen
// sinyalde işlenen istek yoktur: süreç hemen finish(0) ile çıkar.
void on_begin(std::function<void()> fn);

// Kapanma istendi mi (döngüler yeni iş almamak için bakar).
bool requested();

// Bitiş: veritabanı havuzlarını kapatır, günlüğe yazar, çıkar (yıkıcılar koşmaz — çıkışta
// kilit bekleyen bir statik yıkıcı süreci asmasın).
[[noreturn]] void finish(int code);

} // namespace look::shutdown
