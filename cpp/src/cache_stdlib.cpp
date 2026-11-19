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

// Entry cap -- prevents unbounded growth (OOM DoS with attacker-influenced keys).
// Set with LOOK_CACHE_MAX_ENTRIES, default 100k.
// (Consistent with the SSE/WS/Redis/JSON/SMTP caps in the same code base.)
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
    // If a new key would exceed the cap: first drop the expired ones; if still full,
    // random eviction (like Redis allkeys-random) -- memory stays bounded.
    // (Updating an existing key does not grow the cache, so it does not hit the gate.)
    if (store_.find(key) == store_.end()) {
        size_t cap = cache_max_entries();
        if (store_.size() >= cap) evict_expired_locked();
        while (store_.size() >= cap && !store_.empty())
            store_.erase(store_.begin());
    }
    CacheEntry e;
    // ISOLATION (same discipline as parallel deep_clone): the cache is global + cross-thread.
    // If the Value is an ARRAY/MAP it SHARES the shared_ptr<container> -> the vector under
    // the variable that was set would stay the same as the cache's; every request that did a get would
    // share the same vector too -> (1) single thread: a mutation after get silently corrupts the cache
    // (cross-request contamination), (2) multi-thread: concurrent
    // push_back to the shared vector -> heap data race. deep_clone takes a snapshot -> isolated from the caller.
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
    // An ISOLATED copy for every reader (under the lock): getters do not share the same
    // container with each other or with the cache -> a mutation after get corrupts neither the cache nor another reader.
    // All access to the cached vector is under mtx_ (set/get deep_clone) -> no race.
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
        if (args.size() < 2) throw std::runtime_error("cache::set() — expects (key, value [, ttl])");
        std::string key = args[0].to_string();
        int ttl = (args.size() >= 3) ? args[2].to_int() : 0;
        CacheStore::instance().set(key, args[1], ttl);
        return Value(true);
    };

    // cache::get($key) → value | null
    m.functions["get"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::get() — expects key");
        return CacheStore::instance().get(args[0].to_string());
    };

    // cache::has($key) → bool
    m.functions["has"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::has() — expects key");
        return Value(CacheStore::instance().has(args[0].to_string()));
    };

    // cache::delete($key) → bool (true if existed)
    m.functions["delete"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("cache::delete() — expects key");
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
        auto arr = std::make_shared<ValueArray>();
        for (auto& k : ks) arr->push_back(Value(k));
        return Value(arr);
    };

    return m;
}

} // namespace look
