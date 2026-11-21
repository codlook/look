// Memory budget — look/mem_budget.h
#include "look/mem_budget.h"
#include "look/fiber.h"   // t_preempt_budget, Fiber::current
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#  include <malloc.h>
#  define LOOK_USABLE(p) _msize(p)
#elif defined(__APPLE__)
#  include <malloc/malloc.h>
#  define LOOK_USABLE(p) malloc_size(p)
#else
#  include <malloc.h>
#  define LOOK_USABLE(p) malloc_usable_size(p)
#endif

namespace look {

namespace {
// fiber-ok: the current request's account; Fiber::resume switches to the fiber's own account on every transition
// (Fiber::local.mem_acct) and restores it on return — another suspended request's account is never read.
thread_local MemAccount* t_acct = nullptr;
// fiber-ok: delta not yet written to the process counter; belongs to the thread (total), not to a request.
thread_local int64_t t_delta = 0;

std::atomic<int64_t> g_bytes{0};        // live bytes counted (in batches)
std::atomic<bool>    g_enabled{false};
std::atomic<bool>    g_over{false};     // process limit exceeded: the largest request will be picked
int64_t g_limit = 0, g_req_limit = 0, g_admit = 0;
std::mutex  g_list_mtx;
MemAccount* g_head = nullptr;
constexpr int64_t kFlush = int64_t(1) << 20;   // 1 MiB

// Also written to the request account in batches (64 KiB): reading+writing the account's memory line
// on every allocation cost ~3.5% in an allocation-heavy request in fiber mode; batching wins back half of that
// (measured). The size is counted via the SAME path on allocate and free; batching only thins out the writes.
// Flushed before the account changes (fiber switch, request start/end) and at a safe point.
// fiber-ok: delta not yet written to the current account (t_acct); acct_flush flushes it before t_acct changes.
thread_local int64_t t_acct_delta = 0;
constexpr int64_t kAcctFlush = int64_t(64) << 10;   // 64 KiB
inline void acct_flush() noexcept {
    if (MemAccount* a = t_acct) {
        // Freeing data allocated by another request (shared data) does not push the account below zero:
        // if it did, the request could use memory above its budget.
        int64_t b = a->bytes.load(std::memory_order_relaxed) + t_acct_delta;
        if (b < 0) b = 0;
        a->bytes.store(b, std::memory_order_relaxed);
        if (b > a->limit || a->kill.load(std::memory_order_relaxed)) t_preempt_budget = 0;
    }
    t_acct_delta = 0;
}

// Memory held outside the heap (fiber stack cache, upper bound) and the holder's release hook (mem_budget.h).
std::atomic<int64_t>        g_held{0};
std::atomic<int64_t>        g_stacks{0};   // running fibers' stacks (upper bound)
std::atomic<MemReleaseHook> g_release{nullptr};

inline void on_alloc_sz(int64_t sz) noexcept {
    if ((t_acct_delta += sz) >= kAcctFlush) acct_flush();
    if ((t_delta += sz) >= kFlush) {
        const int64_t now = g_bytes.fetch_add(t_delta, std::memory_order_relaxed) + t_delta;
        t_delta = 0;
        if (g_enabled.load(std::memory_order_relaxed)) {
            if (now > g_limit) {
                g_over.store(true, std::memory_order_relaxed);
                t_preempt_budget = 0;   // let the next safe point check
            }
            const int64_t held = g_held.load(std::memory_order_relaxed);
            if (held > 0 && now + held + g_stacks.load(std::memory_order_relaxed) > g_admit)
                if (MemReleaseHook h = g_release.load(std::memory_order_acquire)) h();
        }
    }
}
inline void on_free_sz(int64_t sz) noexcept {
    if ((t_acct_delta -= sz) <= -kAcctFlush) acct_flush();
    if ((t_delta -= sz) <= -kFlush) {
        g_bytes.fetch_add(t_delta, std::memory_order_relaxed);
        t_delta = 0;
    }
}

std::string read_file_trim(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::string();
    std::string s; std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
// Container (cgroup) memory limit: the smallest among this process's cgroup and its ancestors; 0 if none.
int64_t cgroup_memory_limit() {
#ifdef __linux__
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    int64_t best = 0;
    auto take = [&best](const std::string& v, int64_t unlimited_from) {
        if (v.empty() || v == "max") return;
        char* end = nullptr;
        long long n = std::strtoll(v.c_str(), &end, 10);
        if (end == v.c_str() || n <= 0 || n >= unlimited_from) return;
        if (best == 0 || n < best) best = n;
    };
    while (std::getline(cg, line)) {
        // v2: "0::/system.slice/look-x.service"; v1: "4:memory:/docker/…"
        size_t c1 = line.find(':'), c2 = line.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        std::string ctrl = line.substr(c1 + 1, c2 - c1 - 1), path = line.substr(c2 + 1);
        std::string base, file;
        if (ctrl.empty() && line.compare(0, 1, "0") == 0) { base = "/sys/fs/cgroup"; file = "memory.max"; }
        else if (ctrl.find("memory") != std::string::npos) { base = "/sys/fs/cgroup/memory"; file = "memory.limit_in_bytes"; }
        else continue;
        // its own cgroup and its ancestors (systemd slice, container root)
        for (std::string p = path;; ) {
            take(read_file_trim(base + (p == "/" ? "" : p) + "/" + file), int64_t(1) << 60);
            if (p.empty() || p == "/") break;
            size_t s = p.find_last_of('/');
            p = (s == 0 || s == std::string::npos) ? "/" : p.substr(0, s);
        }
    }
    return best;
#else
    return 0;
#endif
}
// Go GOMEMLIMIT format: bytes or a B/KiB/MiB/GiB/TiB suffix; "off" = no limit.
bool parse_limit(const std::string& s, int64_t& out) {
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == 0 || i > 18) return false;
    const int64_t n = std::strtoll(s.substr(0, i).c_str(), nullptr, 10);
    const std::string u = s.substr(i);
    int64_t mul;
    if (u.empty() || u == "B") mul = 1;
    else if (u == "KiB") mul = int64_t(1) << 10;
    else if (u == "MiB") mul = int64_t(1) << 20;
    else if (u == "GiB") mul = int64_t(1) << 30;
    else if (u == "TiB") mul = int64_t(1) << 40;
    else return false;
    if (n > (int64_t(1) << 62) / mul) return false;
    out = n * mul;
    return out > 0;
}
} // namespace

std::string mem_format_bytes(int64_t b) {
    char buf[48];
    if (b >= (int64_t(1) << 30) && b % (int64_t(1) << 30) == 0) std::snprintf(buf, sizeof buf, "%lld GiB", (long long)(b >> 30));
    else if (b >= (int64_t(1) << 30)) std::snprintf(buf, sizeof buf, "%.1f GiB", (double)b / (1 << 30));
    else std::snprintf(buf, sizeof buf, "%lld MiB", (long long)((b + (1 << 19)) >> 20));
    return buf;
}

namespace {
// LOOK_MEMORY_LIMIT: 1 = valid value (out), 0 = not given, 2 = "off", -1 = invalid (why).
int read_env_limit(int64_t& out, std::string& why) {
    const char* env = std::getenv("LOOK_MEMORY_LIMIT");
    if (!env || !*env) return 0;
    if (std::strcmp(env, "off") == 0) return 2;   // no limit — Go GOMEMLIMIT=off
    if (!parse_limit(env, out)) {
        why = std::string("LOOK_MEMORY_LIMIT=") + env + " is not a size: write bytes or a number with "
              "KiB, MiB, GiB or TiB, for example 512MiB or 2GiB (or off)";
        return -1;
    }
    if (out < (int64_t(64) << 20)) {
        why = std::string("LOOK_MEMORY_LIMIT=") + env + " is below 64 MiB; LOOK cannot run in less";
        return -1;
    }
    return 1;
}
bool g_cli = false;   // command line: unaccounted code (the script itself) stops at the process limit
} // namespace

bool mem_budget_configure_cli(std::string& why) {
    int64_t limit = 0;
    const int r = read_env_limit(limit, why);
    if (r < 0) return false;
    if (r != 1) return true;   // not given or off: no limit on the command line
    g_limit = limit;
    g_admit = limit;
    g_req_limit = 0;   // no request budget (MemScope is not set up)
    g_cli = true;
    g_enabled.store(true, std::memory_order_release);
    return true;
}

bool mem_budget_configure(std::string& why, std::string& describe) {
    int64_t limit = 0;
    std::string source;
    const int r = read_env_limit(limit, why);
    if (r < 0) return false;
    if (r == 2) {
        describe = "memory limit off (LOOK_MEMORY_LIMIT=off): one request can use all of the server's memory";
        return true;
    }
    if (r == 1) {
        source = "LOOK_MEMORY_LIMIT";
    } else if (int64_t cg = cgroup_memory_limit()) {
        limit = cg / 10 * 8;
        source = "80% of the container's " + mem_format_bytes(cg);
    } else {
        limit = int64_t(1) << 30;
        source = "no container limit; set LOOK_MEMORY_LIMIT to change it";
    }
    g_limit = limit;
    g_admit = limit / 10 * 9;
    g_req_limit = std::min<int64_t>(int64_t(256) << 20, limit / 2);
    g_enabled.store(true, std::memory_order_release);
    describe = "memory limit " + mem_format_bytes(limit) + " (" + source + "), " +
               mem_format_bytes(g_req_limit) + " per request";
    return true;
}

bool    mem_budget_enabled() { return g_enabled.load(std::memory_order_acquire); }
int64_t mem_process_limit()  { return g_limit; }
int64_t mem_request_limit()  { return g_req_limit; }
int64_t mem_process_bytes()  { return g_bytes.load(std::memory_order_relaxed) + t_delta; }

// A request is admitted against the counted heap only: its connection's stack is already paid for.
bool mem_admit() {
    return !mem_budget_enabled() || mem_process_bytes() < g_admit;
}

bool mem_stacks_over() {
    if (!mem_budget_enabled()) return false;
    const int64_t heap = mem_process_bytes(), stacks = g_stacks.load(std::memory_order_relaxed);
    const int64_t held = g_held.load(std::memory_order_relaxed);
    // the cache gives way first (it never makes a connection wait)
    if (held > 0 && heap + stacks + held > g_admit)
        if (MemReleaseHook h = g_release.load(std::memory_order_acquire)) h();
    return heap < g_admit && heap + stacks >= g_admit;
}

void mem_stacks_add(int64_t bytes) noexcept { g_stacks.fetch_add(bytes, std::memory_order_relaxed); }
void mem_held_set(int64_t bytes) noexcept { g_held.store(bytes, std::memory_order_relaxed); }
bool mem_can_hold(int64_t more) noexcept {
    return !mem_budget_enabled() || mem_process_bytes() + g_held.load(std::memory_order_relaxed) +
                                        g_stacks.load(std::memory_order_relaxed) + more <= g_admit;
}
void mem_set_release_hook(MemReleaseHook fn) noexcept { g_release.store(fn, std::memory_order_release); }

MemAccount* mem_current_account() noexcept { return t_acct; }
void        mem_set_current_account(MemAccount* a) noexcept { acct_flush(); t_acct = a; }   // delta goes to the old account

MemScope::MemScope() {
    // When disabled or when there is no request budget (command line) no account is set up; the destructor looks at limit==0.
    if (!mem_budget_enabled() || g_req_limit <= 0) return;
    acct_.limit = g_req_limit;
    {
        std::lock_guard<std::mutex> lk(g_list_mtx);
        acct_.next = g_head;
        if (g_head) g_head->prev = &acct_;
        g_head = &acct_;
    }
    prev_ = t_acct;
    acct_flush();   // the previous account's delta goes to it
    t_acct = &acct_;
    if (Fiber* f = Fiber::current()) f->local.mem_acct = &acct_;
}
MemScope::~MemScope() {
    if (!acct_.limit) return;   // had been set up while the budget was disabled
    acct_flush();
    t_acct = prev_;
    if (Fiber* f = Fiber::current()) f->local.mem_acct = prev_;
    std::lock_guard<std::mutex> lk(g_list_mtx);
    if (acct_.prev) acct_.prev->next = acct_.next; else g_head = acct_.next;
    if (acct_.next) acct_.next->prev = acct_.prev;
}

namespace {
[[noreturn]] void throw_request_over(const char* what, int64_t need) {
    std::string m = what ? std::string(what) + " would need " + mem_format_bytes(need) + ": the request"
                         : std::string("the request");
    throw MemoryLimitError(m + " would use more than " + mem_format_bytes(g_req_limit) +
        " of memory and was stopped. One request may use half of the memory limit (" +
        mem_format_bytes(g_limit) + ", LOOK_MEMORY_LIMIT), at most 256 MiB; keep large data out of "
        "memory (stream it, page through it, or store it)", false);
}
// A request stopped midway does not roll back what it already did (database writes ...) — stated explicitly in the log.
[[noreturn]] void throw_process_over(const char* what, int64_t used) {
    throw MemoryLimitError((what ? std::string(what) + ": t" : std::string("T")) +
        "he server reached its memory limit (" + mem_format_bytes(g_limit) +
        ", LOOK_MEMORY_LIMIT) and this request, using the most memory (" + mem_format_bytes(used) +
        "), was stopped midway so that the others can finish; what it did before stopping (for example "
        "database writes) is not undone. The client got 503 and may retry", true);
}
// Command line (or non-request code): process limit.
[[noreturn]] void throw_program_over(const char* what, int64_t need) {
    throw MemoryLimitError((what ? std::string(what) + " would need " + mem_format_bytes(need) + ", but t"
                                 : std::string("T")) +
        "he program reached its memory limit (" + mem_format_bytes(g_limit) +
        ", LOOK_MEMORY_LIMIT) and was stopped", true);
}
// Process limit: the request using the MOST memory is picked (not the one allocating at that moment). The picked one
// returns; it stops at its own safe point, and once its memory is released the others carry on.
MemAccount* kill_biggest() {
    std::lock_guard<std::mutex> lk(g_list_mtx);
    MemAccount* big = nullptr;
    for (MemAccount* x = g_head; x; x = x->next)
        if (!big || x->bytes.load(std::memory_order_relaxed) > big->bytes.load(std::memory_order_relaxed)) big = x;
    if (big) big->kill.store(true, std::memory_order_relaxed);
    return big;
}
} // namespace

void mem_check() {
    acct_flush();
    MemAccount* a = t_acct;
    if (!a) {
        // Command line: the script itself stops at the process limit. In the web runtime, unaccounted code (setup,
        // timer) is not stopped here; the largest REQUEST is picked at the process limit.
        if (g_cli && mem_process_bytes() > g_limit) throw_program_over(nullptr, 0);
        return;
    }
    if (g_over.load(std::memory_order_relaxed)) {
        if (mem_process_bytes() > g_limit) kill_biggest();
        g_over.store(false, std::memory_order_relaxed);
    }
    if (a->kill.load(std::memory_order_relaxed)) throw_process_over(nullptr, a->bytes.load(std::memory_order_relaxed));
    if (a->bytes.load(std::memory_order_relaxed) > a->limit) throw_request_over(nullptr, 0);
}

void mem_reserve(int64_t bytes, const char* what) {
    if (!mem_budget_enabled() || bytes <= 0) return;
    acct_flush();
    MemAccount* a = t_acct;
    if (a && a->bytes.load(std::memory_order_relaxed) + bytes > a->limit) throw_request_over(what, bytes);
    // Process limit: the largest request stops, not the one that allocates (same rule as mem_check).
    // If the caller is the largest or has no account (non-request), the allocation is not refused.
    if (mem_process_bytes() + bytes > g_limit) {
        if (!a) throw_program_over(what, bytes);
        if (kill_biggest() == a) throw_process_over(what, a->bytes.load(std::memory_order_relaxed));
    }
    if (a && a->kill.load(std::memory_order_relaxed))
        throw_process_over(what, a->bytes.load(std::memory_order_relaxed));
}

} // namespace look

// ── Every allocation is counted ───────────────────────────────────────────────────────
// Counting is exception-free (noexcept) — the error is raised at a safe point. The two entry paths (operator
// new/delete in release, allocator hooks under TSan/ASan) call ONLY on_alloc_sz/on_free_sz: the counters,
// limit flags, mem_check and kill_biggest are shared — the sanitizer build tests the release logic; the only difference
// is the source of the size (malloc_usable_size vs. requested size).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define LOOK_MEM_SANITIZER_HOOKS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define LOOK_MEM_SANITIZER_HOOKS 1
#  endif
#endif

#ifdef LOOK_MEM_SANITIZER_HOOKS
// TSan/ASan define operator new/delete in their own runtime (statically): a second definition
// breaks linking. In these builds counting is done through the runtime's malloc/free hooks — the budget really works
// under TSan/ASan too (memory_budget_test runs there as well).
// The sanitizer's stable C interface (sanitizer/allocator_interface.h) — the header is not
// installed in every toolchain (missing in gcc-toolset-12), so it is declared here.
extern "C" {
int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, std::size_t),
                                               void (*free_hook)(const volatile void*));
std::size_t __sanitizer_get_allocated_size(const volatile void* p);
}
namespace {
void look_san_malloc_hook(const volatile void* p, std::size_t n) {
    if (p) look::on_alloc_sz((int64_t)n);
}
void look_san_free_hook(const volatile void* p) {
    if (p) look::on_free_sz((int64_t)__sanitizer_get_allocated_size(p));
}
struct LookSanHooks {
    LookSanHooks() { __sanitizer_install_malloc_and_free_hooks(look_san_malloc_hook, look_san_free_hook); }
} g_look_san_hooks;
} // namespace
#else
// on top of malloc/free; the size is the real size given by the allocator (malloc_usable_size).
static void* look_counted_new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    look::on_alloc_sz((int64_t)LOOK_USABLE(p));
    return p;
}
static void* look_counted_new_nothrow(std::size_t n) noexcept {
    void* p = std::malloc(n ? n : 1);
    if (p) look::on_alloc_sz((int64_t)LOOK_USABLE(p));
    return p;
}
static void look_counted_delete(void* p) noexcept {
    if (!p) return;
    look::on_free_sz((int64_t)LOOK_USABLE(p));
    std::free(p);
}
void* operator new(std::size_t n) { return look_counted_new(n); }
void* operator new[](std::size_t n) { return look_counted_new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return look_counted_new_nothrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return look_counted_new_nothrow(n); }
void operator delete(void* p) noexcept { look_counted_delete(p); }
void operator delete[](void* p) noexcept { look_counted_delete(p); }
void operator delete(void* p, std::size_t) noexcept { look_counted_delete(p); }
void operator delete[](void* p, std::size_t) noexcept { look_counted_delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { look_counted_delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { look_counted_delete(p); }
#endif
