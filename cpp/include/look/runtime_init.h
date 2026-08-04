#pragma once
namespace look {

// Süreç başlangıç init'i — TÜM giriş noktaları (lk / lk-fcgi / lk-cgi) bunu çağırır.
// Yeni bir startup işi buraya eklenir → ÜÇ binary de otomatik alır (üç ayrı yere yazmak yok).
//
// NEDEN: aynı "main✅ fcgi✅ cgi❌" deseni İKİ KEZ ısırdı (configure_system_ca_bundle önce,
// sqlite_global_init sonra) — startup işleri üç giriş noktasına AYRI AYRI ekleniyordu ve her
// seferinde biri (cgi) atlanıyordu. Bu birleştirme "kavram başına tek yapı"nın uygulaması ve
// kod azaltır: init listesi tek yerde, üçüncü sapma imkânsız.
void runtime_init();

} // namespace look
