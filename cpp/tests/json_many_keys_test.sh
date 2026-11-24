#!/usr/bin/env bash
# A JSON request body with many keys does not hold a worker: the time grows with the NUMBER of keys, not its square.
#
# json::decode / request::json() found a repeated key by comparing each new key with every key before it (and
# copying each of them): 5,000 keys took 1.1 s, 20,000 keys 3.0 s, 40,000 keys (860 KB) 9.0 s — and the default body
# limit is 10 MB, so one unauthenticated request could keep a worker busy for minutes. The keys of an object are now
# looked up in a hash table kept for that object; "the last value wins, the key keeps its place" is unchanged.
#
# The test posts a 40,000-key JSON object to a route that reads it with request::json() and counts the keys: the
# answer must be right and the request must take less than 3 s (unfixed build: 9 s; fixed: well under 1 s). It also
# checks the duplicate-key rule on a small object, so the speed-up cannot change it.
# Usage: json_many_keys_test.sh <lk-fcgi> [port]     (needs python3, curl)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7983}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
TMP="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$TMP"' EXIT
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
cat > "$TMP/app.lk" <<'LK'
route("POST", "/n", function() {
    $d = request::json()
    $n = 0
    foreach ($d as $k => $v) { $n = $n + 1 }
    return response::text("keys=" . $n . " last=" . $d["key39999"])
})
route("POST", "/dup", function() { return response::text(json::encode(request::json())) })
route("GET", "/ok", function() { return response::text("ok") })
LK
python3 - "$TMP/big.json" <<'PY'
import sys
n = 40000
open(sys.argv[1], "w").write("{" + ",".join('"key%d":"v%d"' % (i, i) for i in range(n)) + "}")
PY
LOOK_MAX_BODY_SIZE=20000000 "$FCGI" --mode http --port "$PORT" "$TMP/app.lk" > "$TMP/log.txt" 2>&1 & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.2; done
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
# -H "Expect:" — curl would wait a second for a 100-continue that this server version does not send
r="$(curl -s -m 60 -H 'Expect:' -H 'Content-Type: application/json' --data-binary @"$TMP/big.json" -w ' %{time_total}' "localhost:$PORT/n")"
body="${r% *}"; t="${r##* }"
check "40,000 keys: all read, the last one right" "$([ "$body" = "keys=40000 last=v39999" ] && echo ok)" "[$body]"
check "40,000 keys: under 3 s (took ${t} s)" "$(python3 -c "import sys; print('ok' if float('$t') < 3.0 else 'no')")" "took ${t} s — the time grows with the square of the keys"
d="$(curl -s -m 10 -H 'Expect:' -H 'Content-Type: application/json' --data-binary '{"a":1,"b":2,"a":3,"c":{"x":1,"x":2,"y":0}}' "localhost:$PORT/dup")"
check "a repeated key: the last value wins and the key keeps its place" "$([ "$d" = '{"a":3,"b":2,"c":{"x":2,"y":0}}' ] && echo ok)" "[$d]"
[ $fail = 0 ] && echo "PASS: JSON bodies with many keys" || { echo "FAIL: JSON bodies with many keys"; exit 1; }
