// cache_stdlib.cpp — cache:: module (in-memory, TTL, thread-safe warm start)
#include "look/cache.h"
#include <algorithm>
#include <cstdlib>

namespace look {

using clock = std::chrono::steady_clock;

// ── Singleton ─────────────────────────────────────────────────────────────
CacheStore& CacheStore::instance() {
    static CacheStore inst;
    return inst;
}

bool CacheStore::is_expired(const CacheEntry& e) const {
    if (!e.has_ttl) return false;
    return clock::now() >= e.expires;
}

void CacheStore::evict_expired_locked() {
    for (auto it = store_.begin(); it != store_.end(); ) {
        if (is_expired(it->second)) it = store_.erase(it);
        else ++it;
    }
}

// Girdi tavanı — sınırsız büyümeyi (saldırgan-etkili anahtarlarla OOM DoS)
// engeller. LOOK_CACHE_MAX_ENTRIES ile ayarlanır, varsayılan 100k.
// (Aynı kod tabanında SSE/WS/Redis/JSON/SMTP tavanlarıyla tutarlı.)
static size_t cache_max_entries() {
    static const size_t v = []() -> size_t {
        const char* e = std::getenv("LOOK_CACHE_MAX_ENTRIES");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return 100000;
    }();
    return v;
}

void CacheStore::set(const std::string& key, Value val, int ttl_seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Yeni anahtar tavanı aşıyorsa: önce süresi dolanları at; hâlâ doluysa
    // rastgele eviction (Redis allkeys-random benzeri) — bellek sınırlı kalır.
    // (Var olan anahtarın güncellenmesi büyümediği için kapıya takılmaz.)
    if (store_.find(key) == store_.end()) {
        size_t cap = cache_max_entries();
        if (store_.size() >= cap) evict_expired_locked();
        while (store_.size() >= cap && !store_.empty())
            store_.erase(store_.begin());
    }
    CacheEntry e;
    // İZOLASYON (parallel deep_clone disiplini ile aynı): cache global + cross-thread.
    // Value ARRAY/MAP ise shared_ptr<container>'ı PAYLAŞIR → set edilen değişkenin
    // altındaki vector cache'le aynı kalırdı; get eden her istek de aynı vector'ü
    // paylaşırdı → (1) tek-thread: get sonrası mutasyon cache'i sessizce bozar
    // (cross-request kontaminasyon), (2) çok-thread: paylaşımlı vector'e eşzamanlı
    // push_back → heap data race. deep_clone snapshot alır → caller'dan izole.
    e.value   = val.deep_clone();
    e.has_ttl = (ttl_seconds > 0);
    if (e.has_ttl)
        e.expires = clock::now() + std::chrono::seconds(ttl_seconds);
    store_[key] = std::move(e);
}

bool CacheStore::has(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;
    if (is_expired(it->second)) { store_.erase(it); return false; }
    return true;
}

Value CacheStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return Value();
    if (is_expired(it->second)) { store_.erase(it); return Value(); }
    // Her okuyucuya İZOLE kopya (kilit altında): getter'lar birbiriyle ve cache'le
    // aynı container'ı paylaşmaz → get sonrası mutasyon ne cache'i ne diğer okuyucuyu
    // bozar. Cached vector'e tüm erişim mtx_ altında (set/get deep_clone) → yarış yok.
    return it->second.value.deep_clone();
}

bool CacheStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    return store_.erase(key) > 0;
}

void CacheStore::flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    store_.clear();
}

int CacheStore::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    evict_expired_locked();
    return (int)store_.size();
}

std::vector<std::string> CacheStore::keys() {
    std::lock_guard<std::mutex> lk(mtx_);
    evict_expired_locked();
    std::vector<std::string> result;
    result.reserve(store_.size());
    for (auto& [k, _] : store_) result.push_back(k);
    std::sort(result.begin(), result.end());
    return result;
}

// ── LOOK module ────────────────────────────────────────────────────────────
Module make_cache_module() {
    Module m;
    m.name = "cache";

    // cache::set($key, $val [, $ttl_seconds])
    // TTL = 0 → no expiry
    m.functions["set"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("cache::set() — (key, value [, ttl]) bekler");
        std::string key = args[0].to_string();
        int ttl = (args.size() >= 3) ? args[2].to_int() : 0;
        CacheStore::instance().set(key, args[1], ttl);
        return Value(true);
    };

    // cache::get($key) → value | null
    m.functions["get"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::get() — key bekler");
        return CacheStore::instance().get(args[0].to_string());
    };

    // cache::has($key) → bool
    m.functions["has"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::has() — key bekler");
        return Value(CacheStore::instance().has(args[0].to_string()));
    };

    // cache::delete($key) → bool (true if existed)
    m.functions["delete"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::delete() — key bekler");
        return Value(CacheStore::instance().del(args[0].to_string()));
    };

    // cache::flush() → deletes all entries
    m.functions["flush"] = [](std::vector<Value> args) -> Value {
        CacheStore::instance().flush();
        return Value(true);
    };

    // cache::size() → int
    m.functions["size"] = [](std::vector<Value> args) -> Value {
        return Value(CacheStore::instance().size());
    };

    // cache::keys() → string[]
    m.functions["keys"] = [](std::vector<Value> args) -> Value {
        auto ks = CacheStore::instance().keys();
        auto arr = std::make_shared<std::vector<Value>>();
        for (auto& k : ks) arr->push_back(Value(k));
        return Value(arr);
    };

    return m;
}

} // namespace look
