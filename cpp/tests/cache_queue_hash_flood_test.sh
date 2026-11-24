#!/usr/bin/env bash
# cache:: keys and queue:: names that share one hash value do not make every call walk one chain (hash flooding).
#
# CacheStore and QueueStore keep their entries in a std::unordered_map. With the C++ library's std::hash<std::string>
# (fixed seed) keys made to share one hash value (see hash_collide_gen.cpp) land in one bucket, and every set walks
# the whole chain: measured on 1.1.6, 40,000 colliding keys took 31 s in cache::set and 9.5 s in queue::push, with the
# store's mutex held (every other cache user waits). An application is open to this when it puts text from a request
# into a cache key or a queue name. The maps are hashed with look_key_hash (SipHash with a per-process seed) now.
#
# The test makes 30,000 colliding keys and sets them with cache::set and queue::push: each loop must take under
# 1.5 s (the library hash: about 15 s and 5 s).
# Usage: cache_queue_hash_flood_test.sh <lk>     (needs g++ with libstdc++)
HERE="$(cd "$(dirname "$0")" && pwd)"
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
KEYS="${KEYS:-30000}"
command -v g++ >/dev/null 2>&1 || { echo "  (skipped: g++ is required to make the colliding keys)"; exit 0; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
g++ -O2 -std=c++17 "$HERE/hash_collide_gen.cpp" -o "$TMP/gen" 2>"$TMP/gen.err" || { echo "  (skipped: the generator does not compile here)"; exit 0; }
"$TMP/gen" "$KEYS" "$TMP/collide.json" 2>"$TMP/gen.err"; rc=$?
[ $rc = 0 ] || { echo "  (skipped: the keys do not collide with this C++ library's std::hash — nothing to attack)"; exit 0; }
cat > "$TMP/t.lk" <<'LK'
use json
use file
use cache
use queue
use date
$keys = json::decode(file::read("collide.json"))
$n = 0
$t0 = date::mono_us()
foreach ($keys as $k => $v) { cache::set($k, 1); $n = $n + 1 }
print("cache_ms=" . ((date::mono_us() - $t0) / 1000) . " cached=" . cache::size())
$t0 = date::mono_us()
foreach ($keys as $k => $v) { queue::push($k, 1) }
print("queue_ms=" . ((date::mono_us() - $t0) / 1000) . " queues=" . count(queue::names()))
LK
export LOOK_FILE_ROOT="$TMP" LOOK_CACHE_MAX_ENTRIES=1000000
out="$(cd "$TMP" && "$LK" t.lk 2>&1)"
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
c_ms="$(echo "$out" | sed -n 's/^cache_ms=\([0-9]*\).*/\1/p')"; q_ms="$(echo "$out" | sed -n 's/^queue_ms=\([0-9]*\).*/\1/p')"
check "$KEYS colliding cache keys: all stored" "$(echo "$out" | grep -q "cached=$KEYS\$" && echo ok)" "[$out]"
check "$KEYS colliding queue names: all stored" "$(echo "$out" | grep -q "queues=$KEYS\$" && echo ok)" "[$out]"
check "cache::set under 1.5 s (took ${c_ms:-?} ms)" "$([ -n "$c_ms" ] && [ "$c_ms" -lt 1500 ] && echo ok)" "each set walks one chain (hash flooding)"
check "queue::push under 1.5 s (took ${q_ms:-?} ms)" "$([ -n "$q_ms" ] && [ "$q_ms" -lt 1500 ] && echo ok)" "each push walks one chain (hash flooding)"
[ $fail = 0 ] && echo "PASS: cache/queue hash flooding" || { echo "FAIL: cache/queue hash flooding"; exit 1; }
