#!/usr/bin/env bash
# Dropping a deeply nested value never takes the process down.
#
# Arrays/structs nested inside each other, or a chain of closures each capturing the previous one,
# were released recursively in C++ (vector -> Value -> vector ...). Measured on the old binary:
# about 200k levels overflowed the C++ stack when the value was simply dropped — one request
# killed the whole server (every site on the binary), on fibers and on the worker pool alike.
# Now the storage (ValueArray, Closure) releases like CPython's "trashcan": past a fixed depth the
# elements are moved to a list and destroyed in a loop, so stack use no longer depends on depth.
# Dropping is unbounded and never fails. Also checked: no source line builds array storage as a
# plain std::vector<Value> (that storage would skip the safe release).
# Usage: value_release_test.sh <lk> <lk-fcgi>
SRC="$(cd "$(dirname "$0")/.." && pwd)"
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
cd "$TMP"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# Source guard: array/struct storage is made only as ValueArray.
plain=$(grep -rnE "make_shared<std::vector<(look::)?Value>>" "$SRC/src" "$SRC/include" 2>/dev/null | grep -v "/sqlite3/")
if [ -z "$plain" ]; then ok "no array storage built as a plain std::vector<Value>"
else bad "plain std::vector<Value> storage (use ValueArray):"; echo "$plain" | sed 's/^/       /'; fi

# case: label | LOOK program | expected last line
run() {
    printf '%s\n' "$2" > t.lk
    out=$(timeout 120 "$LK" t.lk 2>&1); rc=$?
    if [ $rc -ge 128 ]; then bad "$1: process died (exit $rc)"; return; fi
    last=$(echo "$out" | tail -1)
    [ "$last" = "$3" ] && ok "$1" || bad "$1: got [$(echo "$out" | tail -c 200 | tr '\n' ' ')], want [$3]"
}
run "drop 1M nested arrays" '$a = []
for ($i = 0; $i < 1000000; $i = $i + 1) { $a = [$a] }
$a = null
print("dropped")' "dropped"
run "drop 1M nested assoc arrays" '$a = []
for ($i = 0; $i < 1000000; $i = $i + 1) { $a = ["k" => $a, "n" => $i] }
$a = null
print("dropped")' "dropped"
run "drop 1M chained closures" '$f = fn() => 0
for ($i = 0; $i < 1000000; $i = $i + 1) { $g = $f; $f = fn() => $g }
$f = null
print("dropped")' "dropped"
run "drop 1M chained structs" 'struct N { next any }
$n = null
for ($i = 0; $i < 1000000; $i = $i + 1) { $n = N{next: $n} }
$n = null
print("dropped")' "dropped"
run "dropped at script end" '$a = []
for ($i = 0; $i < 1000000; $i = $i + 1) { $a = [$a] }
print("end")' "end"
run "a live copy keeps the value" '$a = []
for ($i = 0; $i < 300000; $i = $i + 1) { $a = [$a, $i] }
$keep = $a
$a = null
print($keep[1])
$keep = null
print("ok")' "ok"
run "wide arrays still work" '$a = []
for ($i = 0; $i < 200000; $i = $i + 1) { $a[$i] = [$i, "x" . $i] }
print(count($a) . " " . $a[199999][1])' "200000 x199999"

# Server: building and dropping a deep value in a request must not take the process down.
PORT="${PORT:-7822}"
cat > app.lk <<'LK'
route("GET", "/drop", function() { $a = []; for ($i = 0; $i < 1000000; $i = $i + 1) { $a = [$a] } return response::text("built") })
route("GET", "/ok", fn() => response::text("alive"))
LK
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
    ( exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    got="$(curl -s -m 60 "localhost:$PORT/drop") $(curl -s -m 5 "localhost:$PORT/ok")"
    up=no; kill -0 "$pid" 2>/dev/null && up=yes
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$got" = "built alive" ] && [ $up = yes ] && ok "[${mode:-default}] server: deep value dropped in a request, server up" \
        || bad "[${mode:-default}] server: got [$got] up: $up"
done

[ $fail = 0 ] && echo "PASS: deeply nested values are released without a crash" || { echo "FAIL: value release"; exit 1; }
