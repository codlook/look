#!/usr/bin/env bash
# LOOK 2: two kinds of code that silently did nothing now stop the application at startup, with
# the reason — the same as a call to a function that does not exist.
#
#   1. The result of a pure built-in is thrown away: `array::push($list, $x)` alone on a line.
#      array::push returns a new array and does not change $list, so the line did nothing and
#      nothing said so. Built-ins that take a callback (array::map …) or that are called to see if
#      they throw (json::decode …) are not in this rule and still start.
#   2. A route on the path the server answers itself (GET /health, or LOOK_HEALTH_PATH): the
#      server replies before the application runs, so the route never ran.
# Usage: silent_noop_startup_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7753}"
# Git Bash (Windows) would rewrite LOOK_HEALTH_PATH=/hc into a Windows path for a native .exe.
export MSYS2_ENV_CONV_EXCL="LOOK_HEALTH_PATH"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# start <dir> [env…]: up=1 if it serves (then stop it), else rc = its exit code
start() {
    local d="$1"; shift
    ( cd "$d" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    up=0; rc=""
    for i in $(seq 1 80); do
        curl -s -o /dev/null -m 1 "localhost:$PORT/x" && { up=1; break; }
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ $up = 1 ]; then kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    else wait "$pid" 2>/dev/null; rc=$?; fi
    pid=""
}
mk() { mkdir -p "$TMP/$1"; cat > "$TMP/$1/app.lk"; }
refused() {  # refused <case> <dir> <text in the log>
    if [ $up = 0 ] && [ "$rc" = 1 ] && grep -qF "$3" "$2/log.txt"; then ok "$1: does not start, says why"
    else bad "$1: up=$up rc=$rc log=[$(grep -i error "$2/log.txt" | head -1)]"; fi
}
starts() { [ $up = 1 ] && ok "$1: starts" || bad "$1: did not start: [$(grep -i error "$2/log.txt" | head -1)]"; }

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
start "$TMP/push_route"
refused "array::push without its result, inside a route" "$TMP/push_route" "line 5: the result of array::push() is not used"

mkdir -p "$TMP/push_inc"; printf 'use string\nfunction tidy($s) {\n    string::trim($s)\n    return $s\n}\n' > "$TMP/push_inc/lib.lk"
mk push_inc <<'LK'
use "lib.lk"
route("GET", "/x", fn() => response::text(tidy(" a ")))
LK
start "$TMP/push_inc"
refused "a discarded string::trim in an included file" "$TMP/push_inc" 'use "lib.lk": line 3: the result of string::trim() is not used'

mk push_ok <<'LK'
use array
use json
$notes = []
$notes = array::push($notes, 1)
array::map($notes, fn($n) => $n)
try { json::decode("{bad") } catch ($e) { }
push($notes, 2)
route("GET", "/x", fn() => response::text("x"))
LK
start "$TMP/push_ok"
starts "the result assigned; array::map, json::decode and push() (changes its array) alone on a line" "$TMP/push_ok"

# the CLI too: it does not run the program on its fallback engine
printf 'use array\n$a = [1]\narray::push($a, 2)\nprint("ran")\n' > "$TMP/cli.lk"
out="$(cd "$TMP" && "$LK" cli.lk 2>&1)"; crc=$?
if [ $crc = 1 ] && echo "$out" | grep -qF "line 3: the result of array::push() is not used" && ! echo "$out" | grep -q '^ran'; then
    ok "lk: the same program is refused before it runs"
else bad "lk: rc=$crc out=[$out]"; fi

# a bare built-in too: strtoupper() returns the new string and changes nothing
printf '$s = "a"\nstrtoupper($s)\nprint("ran")\n' > "$TMP/cli2.lk"
out="$(cd "$TMP" && "$LK" cli2.lk 2>&1)"; crc=$?
if [ $crc = 1 ] && echo "$out" | grep -qF "line 2: the result of strtoupper() is not used" && ! echo "$out" | grep -q '^ran'; then
    ok "lk: a discarded strtoupper() is refused too"
else bad "lk strtoupper: rc=$crc out=[$out]"; fi

# ── 2. a route on the built-in health path ───────────────────────────────────────────────────
mk health <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("GET", "/health", fn() => response::json(["ok" => true]))
LK
start "$TMP/health"
refused "GET /health" "$TMP/health" "route GET /health is never reached"

mk health_post <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("POST", "/health", fn() => response::text("posted"))
route("GET", "/{page}", function($page) { return response::text($page) })
LK
start "$TMP/health_post"
starts "POST /health and a /{page} pattern" "$TMP/health_post"

start "$TMP/health" LOOK_HEALTH_PATH=/hc
starts "GET /health when LOOK_HEALTH_PATH moved the health check" "$TMP/health"

mk health_moved <<'LK'
route("GET", "/x", fn() => response::text("x"))
route("GET", "/hc", fn() => response::text("mine"))
LK
start "$TMP/health_moved" LOOK_HEALTH_PATH=/hc
refused "a route on the moved health path" "$TMP/health_moved" "route GET /hc is never reached"

[ $fail = 0 ] && echo "PASS: code that silently did nothing stops the application at startup" \
    || { echo "FAIL: silent no-op startup checks"; exit 1; }
