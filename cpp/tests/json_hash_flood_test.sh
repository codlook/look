#!/usr/bin/env bash
# A JSON request body whose keys all share one hash value does not hold a worker (hash flooding).
#
# json::decode (request::json()) looks a repeated key up in a table kept for the object. With std::unordered_map
# that table is hashed with std::hash<std::string>, whose seed is FIXED: keys made to share one hash value (see
# hash_collide_gen.cpp) land in one bucket and every lookup walks the whole chain — the time grows with the
# square of the number of keys again (measured on 1.1.5: 40,000 such keys, a 1.2 MB body, took 8.2 s; 40,000 ordinary
# keys 0.02 s; the default body limit is 10 MB). The table is an ordered tree now (std::map): the worst case
# is O(log n) per key whatever the keys are.
#
# The test makes 30,000 colliding keys with the generator, posts them to a route that reads the body with
# request::json() and counts the keys: the answer must be right and the request must take under 1.5 s (an
# unordered_map build: about 4.5 s; the tree: well under 0.5 s).
# Usage: json_hash_flood_test.sh <lk-fcgi> [port]     (needs g++ with libstdc++, python3, curl)
HERE="$(cd "$(dirname "$0")" && pwd)"
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7986}"; KEYS="${KEYS:-30000}"
command -v g++ >/dev/null 2>&1 || { echo "  (skipped: g++ is required to make the colliding keys)"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
TMP="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$TMP"' EXIT
g++ -O2 -std=c++17 "$HERE/hash_collide_gen.cpp" -o "$TMP/gen" 2>"$TMP/gen.err" || { echo "  (skipped: the generator does not compile here)"; exit 0; }
"$TMP/gen" "$KEYS" "$TMP/collide.json" 2>"$TMP/gen.err"; rc=$?
[ $rc = 0 ] || { echo "  (skipped: the keys do not collide with this C++ library's std::hash — nothing to attack)"; exit 0; }
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
cat > "$TMP/app.lk" <<'LK'
route("POST", "/n", function() {
    $d = request::json()
    $n = 0
    foreach ($d as $k => $v) { $n = $n + 1 }
    return response::text("keys=" . $n)
})
route("GET", "/ok", function() { return response::text("ok") })
LK
LOOK_MAX_BODY_SIZE=20000000 "$FCGI" --mode http --port "$PORT" "$TMP/app.lk" > "$TMP/log.txt" 2>&1 & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.2; done
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
# -H "Expect:": curl would wait a second for a 100-continue
r="$(curl -s -m 120 -H 'Expect:' -H 'Content-Type: application/json' --data-binary @"$TMP/collide.json" -w ' %{time_total}' "localhost:$PORT/n")"
body="${r% *}"; t="${r##* }"
check "$KEYS keys with one hash value: all read" "$([ "$body" = "keys=$KEYS" ] && echo ok)" "[$body]"
check "$KEYS keys with one hash value: under 1.5 s (took ${t} s)" "$(python3 -c "print('ok' if float('$t') < 1.5 else 'no')")" "took ${t} s — the lookups walk one chain (hash flooding)"
[ $fail = 0 ] && echo "PASS: JSON hash flooding" || { echo "FAIL: JSON hash flooding"; exit 1; }
