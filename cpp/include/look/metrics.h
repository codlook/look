// metrics.h -- process-lifetime runtime counters (for runtime::stats). Read-only observability.
// Why request_count lives here: the counter used to be a member of the SETUP interpreter; web requests
// go through the VM/per-request copy and never see the setup object -> it would stay 0.
// A process-global atomic is incremented in web dispatch (http_main); runtime::stats reads it. NO format/endpoint/dashboard -- those are packages.
#pragma once
#include <atomic>
#include <cstdint>

namespace look {

// Process-global HTTP request counter incremented by web dispatch.
inline std::atomic<uint64_t>& g_http_request_count() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// In 1.x this was the count of DISTINCT routes permanently dropped from the VM to the interpreter (>0 -> a bug in the VM).
// There is no fallback engine here: no path increments it, runtime::stats reads 0 as "vm_disabled_routes".
inline std::atomic<uint64_t>& g_vm_disabled_routes() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// Number of 5xx responses (silent server errors -- invisible until a user complains).
inline std::atomic<uint64_t>& g_error_5xx_count() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// Total dispatch time (us) + last request (us). avg = sum / request_count. The clocks are already
// being called (t_dispatch_end is unconditional) -> only accumulation; it does NOT ADD a new clock to the hot path.
inline std::atomic<uint64_t>& g_latency_us_sum() {
    static std::atomic<uint64_t> c{0};
    return c;
}
inline std::atomic<uint64_t>& g_latency_us_last() {
    static std::atomic<uint64_t> c{0};
    return c;
}

// Current state of the DB pools (defined in web_stdlib.cpp -- g_pools lives there). total_size = connection count
// of all pools, busy = checked-out. When a pool is full busy->size and requests wait.
void db_pool_stats(int& total_size, int& busy);

} // namespace look
