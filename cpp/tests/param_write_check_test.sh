#!/usr/bin/env bash
# LOOK 2: a function that changes a parameter and never returns it is reported.
#
# Arrays and structs are values: what a function writes into a parameter does not reach the
# caller. In LOOK 1 it did. That difference raises no error — the code runs and the caller
# simply does not see the change — so it is reported instead (look/param_write_check.h):
#   * `lk --check` : an error, one CHECK line per finding, exit status 1
#   * at load (CLI, web startup, files loaded with `use`): one WARN line per finding; the
#     program still runs
# tests/param_write_check/sample.lk marks every line that must be reported with FINDING; no
# other line may be reported.
# Usage: param_write_check_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)/param_write_check"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
fail=0
want="$(grep -n 'FINDING' "$HERE/sample.lk" | grep -v '^1:' | cut -d: -f1 | tr '\n' ' ')"
out="$("$LK" --check "$HERE/sample.lk" 2>/dev/null)"; rc=$?
got="$(echo "$out" | grep '^CHECK .*\[param-write\]' | cut -d' ' -f2 | tr '\n' ' ')"
if [ "$got" = "$want" ] && [ "$rc" = "1" ]; then echo "  OK   --check reports exactly the marked lines ($want) and fails"
else echo "  FAIL --check: want lines [$want] got [$got] exit=$rc"; fail=1; fi
out="$("$LK" --check "$HERE/clean.lk" 2>/dev/null)"; rc=$?
if [ "$out" = "OK" ] && [ "$rc" = "0" ]; then echo "  OK   --check passes a function that returns what it changed"
else echo "  FAIL --check on clean.lk: [$out] exit=$rc"; fail=1; fi
for env in LOOK_VM_STRICT=1 LOOK_CLI_VM=0; do
    o="$(env $env "$LK" "$HERE/sample.lk" 2>"$TMP/err.txt")"; rc=$?
    n="$(grep -c 'WARN.*changes its parameter' "$TMP/err.txt")"; w="$(echo "$want" | wc -w)"
    if [ "$o" = "ran" ] && [ "$rc" = "0" ] && [ "$n" = "$w" ]; then echo "  OK   [$env] at load: $n warnings, the program still runs"
    else echo "  FAIL [$env] at load: output=[$o] exit=$rc warnings=$n (want $w)"; fail=1; fi
done
# web startup and a file loaded with `use`
cat > "$TMP/lib.lk" <<'LK'
function lib_touch($rows) { $rows[0] = "changed" }
LK
cat > "$TMP/app.lk" <<'LK'
use "lib.lk"
function mark($user) { $user["seen"] = true }
route("GET", "/ok", fn() => response::text("served"))
LK
PORT="${PORT:-7735}"
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
body="$(curl -s -m 3 "localhost:$PORT/ok")"
a="$(grep -c 'WARN.*app.lk:2: function mark() changes its parameter \$user' "$TMP/log.txt")"
b="$(grep -c 'WARN.*lib.lk:1: function lib_touch() changes its parameter \$rows' "$TMP/log.txt")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$body" = "served" ] && [ "$a" -ge 1 ] && [ "$b" -ge 1 ]; then echo "  OK   web startup: the application file and the file it loads are both reported; routes are served"
else echo "  FAIL web startup: body=[$body] app-warning=$a lib-warning=$b"; fail=1; fi
[ $fail = 0 ] && echo "PASS: writes into a parameter that never reach the caller are reported" || { echo "FAIL: parameter-write check"; exit 1; }
