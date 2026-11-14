// Bellek bütçesi — look/mem_budget.h
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
// fiber-ok: o anki istek hesabı; Fiber::resume her geçişte fiber'ın kendi hesabına çevirir
// (Fiber::local.mem_acct), dönüşte geri alır — askıdaki başka isteğin hesabı okunmaz.
thread_local MemAccount* t_acct = nullptr;
// fiber-ok: süreç sayacına henüz yazılmamış fark; isteğe ait değil, iş parçacığına ait (toplam).
thread_local int64_t t_delta = 0;

std::atomic<int64_t> g_bytes{0};        // sayılan canlı bayt (partiler hâlinde)
std::atomic<bool>    g_enabled{false};
std::atomic<bool>    g_over{false};     // süreç sınırı aşıldı: en büyük istek seçilecek
int64_t g_limit = 0, g_req_limit = 0, g_admit = 0;
std::mutex  g_list_mtx;
MemAccount* g_head = nullptr;
constexpr int64_t kFlush = int64_t(1) << 20;   // 1 MiB

inline void on_alloc_sz(int64_t sz) noexcept {
    if ((t_delta += sz) >= kFlush) {
        const int64_t now = g_bytes.fetch_add(t_delta, std::memory_order_relaxed) + t_delta;
        t_delta = 0;
        if (now > g_limit && g_enabled.load(std::memory_order_relaxed)) {
            g_over.store(true, std::memory_order_relaxed);
            t_preempt_budget = 0;   // bir sonraki güvenli nokta denetlesin
        }
    }
    if (MemAccount* a = t_acct) {
        const int64_t b = a->bytes.load(std::memory_order_relaxed) + sz;
        a->bytes.store(b, std::memory_order_relaxed);
        if (b > a->limit || a->kill.load(std::memory_order_relaxed)) t_preempt_budget = 0;
    }
}
inline void on_free_sz(int64_t sz) noexcept {
    if ((t_delta -= sz) <= -kFlush) {
        g_bytes.fetch_add(t_delta, std::memory_order_relaxed);
        t_delta = 0;
    }
    if (MemAccount* a = t_acct) {
        // Başka isteğin ayırdığı (paylaşılan) veriyi bırakmak bu hesabı sıfırın altına indirmez:
        // indirseydi istek bütçesinin üstünde bellek kullanabilirdi.
        const int64_t b = a->bytes.load(std::memory_order_relaxed) - sz;
        a->bytes.store(b > 0 ? b : 0, std::memory_order_relaxed);
    }
}

std::string read_file_trim(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::string();
    std::string s; std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
// Kabın (cgroup) bellek sınırı: bu sürecin cgroup'u ve üstleri içinde en küçüğü; yoksa 0.
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
        // kendi cgroup'u ve üstleri (systemd dilimi, kap kökü)
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
// Go GOMEMLIMIT biçimi: bayt ya da B/KiB/MiB/GiB/TiB son eki; "off" = sınır yok.
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
// LOOK_MEMORY_LIMIT: 1 = geçerli değer (out), 0 = verilmemiş, 2 = "off", -1 = geçersiz (why).
int read_env_limit(int64_t& out, std::string& why) {
    const char* env = std::getenv("LOOK_MEMORY_LIMIT");
    if (!env || !*env) return 0;
    if (std::strcmp(env, "off") == 0) return 2;   // sınır yok — Go GOMEMLIMIT=off
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
bool g_cli = false;   // komut satırı: hesapsız kod (betiğin kendisi) süreç sınırında durur
} // namespace

bool mem_budget_configure_cli(std::string& why) {
    int64_t limit = 0;
    const int r = read_env_limit(limit, why);
    if (r < 0) return false;
    if (r != 1) return true;   // verilmemiş ya da off: komut satırında sınır yok
    g_limit = limit;
    g_admit = limit;
    g_req_limit = 0;   // istek bütçesi yok (MemScope kurulmaz)
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

bool mem_admit() {
    return !mem_budget_enabled() || mem_process_bytes() < g_admit;
}

MemAccount* mem_current_account() noexcept { return t_acct; }
void        mem_set_current_account(MemAccount* a) noexcept { t_acct = a; }

MemScope::MemScope() {
    // Kapalıyken ya da istek bütçesi yokken (komut satırı) hesap kurulmaz; yıkıcı limit==0'a bakar.
    if (!mem_budget_enabled() || g_req_limit <= 0) return;
    acct_.limit = g_req_limit;
    {
        std::lock_guard<std::mutex> lk(g_list_mtx);
        acct_.next = g_head;
        if (g_head) g_head->prev = &acct_;
        g_head = &acct_;
    }
    prev_ = t_acct;
    t_acct = &acct_;
    if (Fiber* f = Fiber::current()) f->local.mem_acct = &acct_;
}
MemScope::~MemScope() {
    if (!acct_.limit) return;   // bütçe kapalıyken kurulmuştu
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
// Yarıda durdurulan istek o ana dek yaptığını (veritabanı yazması …) geri almaz — günlükte açıkça.
[[noreturn]] void throw_process_over(const char* what, int64_t used) {
    throw MemoryLimitError((what ? std::string(what) + ": t" : std::string("T")) +
        "he server reached its memory limit (" + mem_format_bytes(g_limit) +
        ", LOOK_MEMORY_LIMIT) and this request, using the most memory (" + mem_format_bytes(used) +
        "), was stopped midway so that the others can finish; what it did before stopping (for example "
        "database writes) is not undone. The client got 503 and may retry", true);
}
// Komut satırı (ya da istek dışı kod): süreç sınırı.
[[noreturn]] void throw_program_over(const char* what, int64_t need) {
    throw MemoryLimitError((what ? std::string(what) + " would need " + mem_format_bytes(need) + ", but t"
                                 : std::string("T")) +
        "he program reached its memory limit (" + mem_format_bytes(g_limit) +
        ", LOOK_MEMORY_LIMIT) and was stopped", true);
}
// Süreç sınırı: EN ÇOK bellek kullanan istek seçilir (o an ayırma yapan değil). Seçilen
// döner; kendi güvenli noktasında durur, belleği bırakılınca ötekiler sürer.
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
    MemAccount* a = t_acct;
    if (!a) {
        // Komut satırı: betiğin kendisi süreç sınırında durur. Web'de hesapsız kod (kurulum,
        // zamanlayıcı) burada durdurulmaz; süreç sınırında en büyük İSTEK seçilir.
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
    MemAccount* a = t_acct;
    if (a && a->bytes.load(std::memory_order_relaxed) + bytes > a->limit) throw_request_over(what, bytes);
    // Süreç sınırı: ayırmayı yapan değil, en büyük istek durur (mem_check ile aynı kural).
    // Çağıran en büyükse ya da hesabı yoksa (istek dışı) ayırma yapılmaz.
    if (mem_process_bytes() + bytes > g_limit) {
        if (!a) throw_program_over(what, bytes);
        if (kill_biggest() == a) throw_process_over(what, a->bytes.load(std::memory_order_relaxed));
    }
    if (a && a->kill.load(std::memory_order_relaxed))
        throw_process_over(what, a->bytes.load(std::memory_order_relaxed));
}

} // namespace look

// ── Her ayırma sayılır ───────────────────────────────────────────────────────
// Sayım istisnasız (noexcept) — hata güvenli noktada verilir. İki giriş yolu (sürümde operator
// new/delete, TSan/ASan'da ayırıcı kancaları) YALNIZ on_alloc_sz/on_free_sz'yi çağırır: sayaçlar,
// sınır işaretleri, mem_check ve kill_biggest ortaktır — sanitizer sürümdeki mantığı sınar; fark
// yalnız boyutun kaynağıdır (malloc_usable_size ↔ istenen boyut).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define LOOK_MEM_SANITIZER_HOOKS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define LOOK_MEM_SANITIZER_HOOKS 1
#  endif
#endif

#ifdef LOOK_MEM_SANITIZER_HOOKS
// TSan/ASan operator new/delete'i kendi çalışma zamanında (statik) tanımlar: ikinci tanım bağlamayı
// bozar. Bu derlemelerde sayım çalışma zamanının malloc/free kancalarıyla yapılır — bütçe TSan/ASan
// altında da gerçekten çalışır (memory_budget_test orada da koşar).
// Sanitizer'ın kararlı C arayüzü (sanitizer/allocator_interface.h) — başlık her araç zincirinde
// kurulu değil (gcc-toolset-12'de yok), bildirim burada.
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
// malloc/free üstünde; boyut ayırıcının verdiği gerçek boyuttur (malloc_usable_size).
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
