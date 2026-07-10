#pragma once
#include "interpreter.h"
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <optional>

namespace look {

struct CacheEntry {
    Value                                    value;
    std::chrono::steady_clock::time_point    expires;  // zero = no expiry
    bool                                     has_ttl = false;
};

// Global singleton — survives across FastCGI requests (warm start)
class CacheStore {
public:
    static CacheStore& instance();

    void   set(const std::string& key, Value val, int ttl_seconds = 0);
    bool   has(const std::string& key);
    Value  get(const std::string& key);    // null if missing or expired
    bool   del(const std::string& key);
    void   flush();
    int    size();
    std::vector<std::string> keys();

private:
    std::unordered_map<std::string, CacheEntry> store_;
    mutable std::mutex                           mtx_;

    bool is_expired(const CacheEntry& e) const;
    void evict_expired_locked();
};

Module make_cache_module();

} // namespace look
