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

} // namespace look
