#pragma once
// Map key lookup — O(1) on a large map.
//
// Map storage is unchanged: ["<marker>", k0, v0, k1, v1, …] (order = insertion order; foreach,
// json, array:: all read this layout). OLD: every read and every write scanned the list from the start —
// copying a 40,000-key body in a loop took 4.3 s (quadratic); a single request kept the process busy.
//
// Index: an open-addressed table made of key POSITIONS (uint32; the key text is not copied).
// The hash is SipHash-1-3 with a per-process random seed (as in Rust/Python): keys that land in the same
// cell cannot be produced without knowing the seed (hash flooding).
//
// Validity: the index knows the store length (seen) at the time it was built.
//   * The read path builds the index ONLY from empty (CAS) and never changes it; if the length differs it scans linearly.
//   * The write path (assoc_sync) keeps the store single-owner: it folds appended pairs into the index, and
//     rebuilds if the length shrank.
//   * Every hit is verified against the key text; if it does not match (a mid-store change such as sorting) the write
//     path rebuilds, the read path scans linearly.
// A path that deletes/changes a key in the middle and keeps the length must drop the index (drop_index).
// When built with LOOK_ASSOC_INDEX_VERIFY every indexed lookup is compared with a linear scan
// (tests/assoc_index_test.sh runs all tests in this mode).
#include "look/value.h"
#include <cstdlib>
#include <cstdio>

namespace look {

uint64_t look_key_hash(const char* p, size_t n) noexcept;   // value.cpp (SipHash-1-3, process seed)

// Hash for std::unordered_map<std::string, ...> whose keys can come from a request (cache keys, queue names):
// the C++ library hash has a fixed seed, and keys that share one hash value can be built (hash flooding).
struct KeyHash {
    size_t operator()(const std::string& s) const noexcept { return (size_t)look_key_hash(s.data(), s.size()); }
};

struct AssocIndex {
    std::vector<uint32_t> slots;   // 0 = empty; others are the key's position in the store (odd number ≥ 1)
    size_t  seen = 0;              // store length the index covers
    size_t  next = 1;              // position of the next key to join the index
    size_t  count = 0;
    bool    unusable = false;      // non-text key / old __struct__ marker → linear
};

// The index is not built on a map smaller than this length: a few text comparisons are faster than the table.
constexpr size_t LOOK_ASSOC_INDEX_MIN_PAIRS = 8;

namespace assoc_detail {

inline ValueArray& va(const std::vector<Value>& v) {
    // Every array/map store is a ValueArray (look_new_array / make_shared<ValueArray>;
    // tests/value_release_test.sh rejects a line that builds a plain std::vector<Value>).
    return static_cast<ValueArray&>(const_cast<std::vector<Value>&>(v));
}

// Reading skips the old "__struct__" marker (start 3); writing always scans from 1 (behaviour unchanged).
inline size_t linear(const std::vector<Value>& vec, const std::string& k, bool for_write = false) {
    size_t start = 1;
    if (!for_write && vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__") start = 3;
    for (size_t i = start; i + 1 < vec.size(); i += 2)
        if (vec[i].type() == Value::STRING ? vec[i].str_ref() == k : vec[i].to_string() == k) return i;
    return 0;
}

inline void place(AssocIndex& ix, const std::vector<Value>& vec, size_t pos) {
    const std::string& k = vec[pos].str_ref();
    const size_t mask = ix.slots.size() - 1;
    for (size_t h = (size_t)look_key_hash(k.data(), k.size()) & mask;; h = (h + 1) & mask) {
        uint32_t s = ix.slots[h];
        if (s == 0) { ix.slots[h] = (uint32_t)pos; ++ix.count; return; }
        if (vec[s].str_ref() == k) return;   // duplicate key: the first occurrence wins (as in a linear scan)
    }
}

inline void grow(AssocIndex& ix, const std::vector<Value>& vec, size_t want_pairs) {
    size_t cap = 16;
    while (cap < want_pairs * 2) cap <<= 1;
    if (cap <= ix.slots.size()) return;
    std::vector<uint32_t> old;
    old.swap(ix.slots);
    ix.slots.assign(cap, 0);
    ix.count = 0;
    for (uint32_t s : old) if (s) place(ix, vec, s);
}

// Fold pairs appended at the end into the index (seen/next advance). Non-text key → not usable.
inline void extend(AssocIndex& ix, const std::vector<Value>& vec) {
    size_t i = ix.next;
    if (vec.size() > ix.seen) grow(ix, vec, ix.count + (vec.size() - ix.seen) / 2 + 1);
    for (; i + 1 < vec.size(); i += 2) {
        if (vec[i].type() != Value::STRING || vec[i].is_assoc_marker()) { ix.unusable = true; break; }
        place(ix, vec, i);
    }
    ix.next = i;
    ix.seen = vec.size();
}

inline AssocIndex* build(const std::vector<Value>& vec) {
    auto* ix = new AssocIndex();
    if (vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__") {
        ix->unusable = true; ix->seen = vec.size();
        return ix;
    }
    extend(*ix, vec);
    return ix;
}

inline size_t probe(const AssocIndex& ix, const std::vector<Value>& vec, const std::string& k) {
    const size_t mask = ix.slots.size() - 1;
    for (size_t h = (size_t)look_key_hash(k.data(), k.size()) & mask;; h = (h + 1) & mask) {
        uint32_t s = ix.slots[h];
        if (s == 0) return 0;
        if (s < vec.size() && vec[s].type() == Value::STRING && vec[s].str_ref() == k) return s;
        if (s >= vec.size() || vec[s].type() != Value::STRING) return SIZE_MAX;   // stale
        // another key that fell in the same cell: next cell
    }
}

#ifdef LOOK_ASSOC_INDEX_VERIFY
inline size_t verified(const std::vector<Value>& vec, const std::string& k, size_t got) {
    size_t want = linear(vec, k);
    if (got != want) {
        std::fprintf(stderr, "ASSOC INDEX MISMATCH: key [%s] index %zu linear %zu size %zu\n",
                     k.c_str(), got, want, vec.size());
        std::abort();
    }
    return got;
}
#else
inline size_t verified(const std::vector<Value>&, const std::string&, size_t got) { return got; }
#endif

} // namespace assoc_detail

// POSITION of key k in the map (value at position+1); 0 if absent. vec must be a map (vec[0] is the marker).
// Does not change the store (read path; several threads may call it on a shared map).
inline size_t assoc_find(const std::vector<Value>& vec, const std::string& k) {
    using namespace assoc_detail;
    if (vec.size() < 1 + 2 * LOOK_ASSOC_INDEX_MIN_PAIRS) return linear(vec, k);
    ValueArray& a = va(vec);
    AssocIndex* ix = a.assoc_idx.load(std::memory_order_acquire);
    if (!ix) {
        AssocIndex* fresh = build(vec);
        AssocIndex* expected = nullptr;
        if (a.assoc_idx.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            ix = fresh;
        else { look_assoc_index_free(fresh); ix = expected; }
    }
    if (ix->unusable || ix->seen != vec.size()) return linear(vec, k);
    size_t p = probe(*ix, vec, k);
    if (p == SIZE_MAX) return linear(vec, k);
    return verified(vec, k, p);
}

// Write path: the store is single-owner (copy on write). Syncs the index with the store, then searches.
inline size_t assoc_find_for_write(std::vector<Value>& vec, const std::string& k) {
    using namespace assoc_detail;
    if (vec.size() < 1 + 2 * LOOK_ASSOC_INDEX_MIN_PAIRS) return linear(vec, k, true);
    ValueArray& a = va(vec);
    AssocIndex* ix = a.assoc_idx.load(std::memory_order_relaxed);
    if (ix && (vec.size() < ix->seen || ix->next > vec.size())) { a.drop_index(); ix = nullptr; }
    if (!ix) { ix = build(vec); a.assoc_idx.store(ix, std::memory_order_release); }
    else if (vec.size() != ix->seen && !ix->unusable) extend(*ix, vec);
    if (ix->unusable) return linear(vec, k, true);
    size_t p = probe(*ix, vec, k);
    if (p == SIZE_MAX) {   // changed in the middle: rebuild
        a.drop_index();
        ix = build(vec); a.assoc_idx.store(ix, std::memory_order_release);
        if (ix->unusable) return linear(vec, k, true);
        p = probe(*ix, vec, k);
        if (p == SIZE_MAX) return linear(vec, k, true);
    }
    return verified(vec, k, p);   // if the index is usable there is no __struct__ marker → both scans are the same
}

// Write path: after assoc_find_for_write returned 0 the caller appended k and the value at the end.
inline void assoc_appended(std::vector<Value>& vec) {
    AssocIndex* ix = assoc_detail::va(vec).assoc_idx.load(std::memory_order_relaxed);
    if (ix && !ix->unusable && ix->seen + 2 == vec.size()) assoc_detail::extend(*ix, vec);
}

} // namespace look
