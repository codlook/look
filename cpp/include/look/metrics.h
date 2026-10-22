// metrics.h — süreç-ömürlü runtime sayaçları (runtime::stats için). Salt-okunur gözlemlenebilirlik.
// request_count neden burada: eskiden sayaç SETUP yorumlayıcısının üyesiydi; web istekleri
// VM/per-request kopya üzerinden gider, setup nesnesini hiç görmez → 0 kalırdı.
// Süreç-global atomik web dispatch'te (http_main) artar; runtime::stats bunu okur. Format/endpoint/dashboard YOK — onlar paket (bkz. monitor kararı).
#pragma once
#include <atomic>
#include <cstdint>

namespace look {

// Web dispatch'in artırdığı süreç-global HTTP istek sayacı.
inline std::atomic<uint64_t>& g_http_request_count() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// LOOK 1'de VM'den kalıcı olarak yorumlayıcıya düşen DISTINCT route sayısıydı (>0 → VM'de bug).
// LOOK 2'de yedek motor yok: hiçbir yol artırmaz, runtime::stats "vm_disabled_routes" olarak 0 okur.
inline std::atomic<uint64_t>& g_vm_disabled_routes() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// 5xx yanıt sayısı (sessiz sunucu hatası — kullanıcı şikâyet edene kadar görünmez).
inline std::atomic<uint64_t>& g_error_5xx_count() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// Dispatch süresi toplamı (us) + son istek (us). avg = sum / request_count. Clock'lar zaten
// çağrılıyor (t_dispatch_end koşulsuz) → sadece toplama; sıcak yola yeni clock EKLEMEZ.
inline std::atomic<uint64_t>& g_latency_us_sum() {
    static std::atomic<uint64_t> c{0};
    return c;
}
inline std::atomic<uint64_t>& g_latency_us_last() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// DB havuzu anlık durumu (web_stdlib.cpp'de tanımlı — g_pools orada). total_size = tüm havuzların
// bağlantı sayısı, busy = checked-out (meşgul). Havuz dolunca busy→size, istekler bekler.
void db_pool_stats(int& total_size, int& busy);

} // namespace look
