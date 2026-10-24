#include "look/runtime_init.h"
#include "look/http_client.h"    // configure_system_ca_bundle
#include "look/sqlite_client.h"  // sqlite_global_init
#ifndef _WIN32
#  include <csignal>
#endif

namespace look {

// Süreç başlangıcında, WORKER THREAD'LERİNDEN ÖNCE bir kez. Sıra önemli değil; ikisi de
// idempotent + thread'lerden önce çalışması gereken bir-kez init'ler.
void runtime_init() {
#ifndef _WIN32
    // Kapanmis bir sokete yazmak SIGPIPE uretir; varsayilan davranis SURECI oldurur. Yalniz
    // `--mode http` yok sayiyordu: MySQL bosta bekleyen havuz baglantisini kapatinca (wait_timeout,
    // yeniden baslatma, KILL) sonraki yazma lk-fcgi'yi FastCGI modunda (Plesk varsayilani) ve
    // `lk`'yi oldurdu (exit 141). Uc giris noktasi da burayi cagirir. Yazma artik EPIPE hatasi
    // doner; DB istemcileri ayrica MSG_NOSIGNAL ile yazar. (`lk` kendi stdout'u kapaninca yine
    // sonlanir — bkz. main.cpp PipeGuard.)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    configure_system_ca_bundle();  // statik OpenSSL: sistem CA'sını bul (Windows'ta no-op — Schannel)
    sqlite_global_init();          // SQLite lazy global init'leri (isInit/PRNG) serialize et (t4 race)
}

} // namespace look
