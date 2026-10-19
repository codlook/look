#!/usr/bin/env bash
# Two kinds of code that silently did nothing are now reported with a warning at startup. The
# program still runs as before.
#
#   1. The result of a pure built-in is thrown away: `array::push($list, $x)` alone on a line.
#      array::push returns a new array and does not change $list, so the line did nothing and
#      nothing said so. Built-ins that take a callback (array::map …) or that are called to see if
#      they throw (json::decode …) are not in this rule and give no warning.
#   2. A route on the path the server answers itself (GET /health, or LOOK_HEALTH_PATH): the
#      server replies before the application runs, so the route never runs.
# Usage: silent_noop_warning_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7754}"
# Git Bash (Windows) would rewrite LOOK_HEALTH_PATH=/hc into a Windows path for a native .exe.
export MSYS2_ENV_CONV_EXCL="LOOK_HEALTH_PATH"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# serve <dir> [env…]: starts the app, body = GET /x, then stops it
serve() {
    local d="$1"; shift
    ( cd "$d" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    body=""
    for i in $(seq 1 80); do
        body="$(curl -s -m 1 "localhost:$PORT/x")" && [ -n "$body" ] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
}
mk() { mkdir -p "$TMP/$1"; cat > "$TMP/$1/app.lk"; }
warned() {   # warned <case> <dir> <text>
    if [ "$body" = "x" ] && grep -F "[WARN" "$2/log.txt" | grep -qF "$3"; then ok "$1: warned, the application runs"
    else bad "$1: body=[$body] warn=[$(grep -F '[WARN' "$2/log.txt" | grep -v 'Rate limiting\|holds a worker' | head -1)]"; fi
}
quiet() {    # quiet <case> <dir> <text that must not appear>
    if [ "$body" = "x" ] && ! grep -qF "$3" "$2/log.txt"; then ok "$1: no warning"
    else bad "$1: body=[$body] log=[$(grep -F "$3" "$2/log.txt" | head -1)]"; fi
}

# ── 1. discarded result of a pure built-in ───────────────────────────────────────────────────
mk push_route <<'LK'
use array
$notes = []
route("GET", "/x", fn() => response::text("x"))
route("POST", "/n", function() {
    array::push($notes, request::json())
    return response::text("saved")
})
LK
serve "$TMP/push_route"
warned "array::push without its result, inside a route" "$TMP/push_route" "line 5: the result of array::push() is not used"

mkdir -p "$TMP/push_inc"; printf 'use string\nfunction tidy($s) {\n    string::trim($s)\n    return $s\n}\n' > "$TMP/push_inc/lib.lk"
mk push_inc <<'LK'
use "lib.lk"
route("GET", "/x", fn() => response::text("x"))
LK
serve "$TMP/push_inc"
warned "a discarded string::trim in an included file" "$TMP/push_inc" 'use "lib.lk": line 3: the result of string::trim() is not used'

mk push_ok <<'LK'
use array
use json
$notes = []
$notes = array::push($notes, 1)
array::map($notes, fn($n) => $n)
try { json::decode("{bad") } catch ($e) { }
route("GET", "/x", fn() => response::text("x"))
LK
serve "$TMP/push_ok"
quiet "the result assigned; array::map and json::decode alone on a line" "$TMP/push_ok" "is not used"

printf 'use array\n$a = [1]\narray::push($a, 2)\nprint("ran")\n' > "$TMP/cli.lk"
out="$(cd "$TMP" && "$LK" cli.lk 2>&1)"; crc=$?
if [ $crc = 0 ] && echo "$out" | grep -qF "Warning: line 3: the result of array::push() is not used" && echo "$out" | grep -q 'ran'; then
    ok "lk: warned, the program runs"
else bad "lk: rc=$crc out=[$out]"; fi

# ── 2. a route on the built-in health path ───────────────────────────────────────────────────
mk health <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("GET", "/health", fn() => response::json(["ok" => true]))
LK
serve "$TMP/health"
warned "GET /health" "$TMP/health" "route GET /health is never reached"

mk health_post <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("POST", "/health", fn() => response::text("posted"))
LK
serve "$TMP/health_post"
quiet "POST /health" "$TMP/health_post" "is never reached"

serve "$TMP/health" LOOK_HEALTH_PATH=/hc
quiet "GET /health when LOOK_HEALTH_PATH moved the health check" "$TMP/health" "is never reached"

mk health_moved <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("GET", "/hc", fn() => response::text("mine"))
LK
serve "$TMP/health_moved" LOOK_HEALTH_PATH=/hc
warned "a route on the moved health path" "$TMP/health_moved" "route GET /hc is never reached"

[ $fail = 0 ] && echo "PASS: code that silently did nothing is reported at startup" \
    || { echo "FAIL: silent no-op warnings"; exit 1; }
