// metrics.h — süreç-ömürlü runtime sayaçları (runtime::stats için). Salt-okunur gözlemlenebilirlik.
// request_count neden burada: interpreter'ın request_count_ üyesi SETUP interpreter'a ait; web
// istekleri VM/per-request kopya üzerinden gider, setup interpreter'ı hiç görmez → 0 kalırdı.
// Süreç-global atomik hem web dispatch'te (http_main) hem interpreter dispatch'te artar; runtime::stats
// bunu okur. Format/endpoint/dashboard YOK — onlar paket (bkz. monitor kararı).
#pragma once
#include <atomic>
#include <cstdint>

namespace look {

// Tüm dispatch yollarının (web + interpreter) artırdığı süreç-global HTTP istek sayacı.
inline std::atomic<uint64_t>& g_http_request_count() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// VM'de kalıcı olarak interpreter'a düşen DISTINCT route sayısı (toplam fallback DEĞİL — route
// başına bir kez, bkz. http_main vm_route_disabled guard'ı). SESSİZ performans kaybı (~%37 yavaş);
// >0 ise VM'de bug var. İsim bilinçli "disabled_routes": "fallbacks" okuyan "N kez düştü" sanırdı.
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
