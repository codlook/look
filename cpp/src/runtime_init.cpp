#include "look/runtime_init.h"
#include "look/http_client.h"    // configure_system_ca_bundle
#include "look/sqlite_client.h"  // sqlite_global_init

namespace look {

// Süreç başlangıcında, WORKER THREAD'LERİNDEN ÖNCE bir kez. Sıra önemli değil; ikisi de
// idempotent + thread'lerden önce çalışması gereken bir-kez init'ler.
void runtime_init() {
    configure_system_ca_bundle();  // statik OpenSSL: sistem CA'sını bul (Windows'ta no-op — Schannel)
    sqlite_global_init();          // SQLite lazy global init'leri (isInit/PRNG) serialize et (t4 race)
}

} // namespace look
