#!/usr/bin/env bash
# LOOK 2: the `use` rule holds in a web application whose top level runs only on the VM.
#
# The top level used to run first on the tree-walk engine, and its `use` lines were what opened a
# module and what rejected a module that does not exist. That pass is gone; the VM now applies the
# same rule:
#   * a module function called without `use <module>` stops the application at startup (top level)
#     or fails the request (route) with "Module 'x' not loaded." — the same as before
#   * `use <module>` in a file loaded with `use "file.lk"` opens the module for the application
#   * `use <name>` for a module that does not exist stops the application: "Unknown module"
#   * `use timer` (a built-in namespace, not a module) stops the application with its own message
# Usage: use_rule_web_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7751}"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# start <dir>: starts the app; sets up=1 if it serves, else waits for it to exit (rc)
start() {
    ( cd "$1" && exec "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    up=0
    for i in $(seq 1 80); do
        curl -s -o /dev/null -m 1 "localhost:$PORT/" && { up=1; return; }
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    wait "$pid" 2>/dev/null; rc=$?; pid=""
}
stop() { [ -n "$pid" ] && { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }; }

mk() { mkdir -p "$TMP/$1"; cat > "$TMP/$1/app.lk"; }

# 1. top level, no `use cache` → does not start
mk top_nouse <<'LK'
cache::set("k", 1, 60)
route("GET", "/", fn() => response::text("started"))
LK
start "$TMP/top_nouse"
if [ $up = 0 ] && [ "$rc" = 1 ] && grep -q "Module 'cache' not loaded" "$TMP/top_nouse/log.txt"; then
    ok "a module function at the top level without its use line: the application does not start"
else bad "top level without use: up=$up rc=${rc:-} log=[$(grep -i error "$TMP/top_nouse/log.txt" | head -1)]"; stop; fi

# 2. the same with `use cache` → starts (positive control)
mk top_use <<'LK'
use cache
cache::set("k", 1, 60)
route("GET", "/", function() { $k = cache::get("k"); return response::text("started:{$k}") })
LK
start "$TMP/top_use"
body="$(curl -s -m 2 "localhost:$PORT/")"; stop
[ "$body" = "started:1" ] && ok "with its use line the same application starts" \
    || bad "top level with use: body=[$body] log=[$(grep -i error "$TMP/top_use/log.txt" | head -1)]"

# 3. route, no `use string` → the request fails with the message; with `use` it works
mk route_nouse <<'LK'
route("GET", "/", fn() => response::text("started"))
route("GET", "/s", function() { try { return response::text(string::upper("x")) } catch ($e) { return response::text("caught:{$e}") } })
LK
start "$TMP/route_nouse"
body="$(curl -s -m 2 "localhost:$PORT/s")"; stop
case "$body" in *"Module 'string' not loaded"*) ok "a module function in a route without its use line fails with the same message";;
    *) bad "route without use: body=[$body]";; esac

# 4. `use string` in an included file opens the module for the application
mk inc_use <<'LK'
use "lib.lk"
route("GET", "/", fn() => response::text(shout("x")))
LK
printf 'use string\nfunction shout($s) { return string::upper($s) }\n' > "$TMP/inc_use/lib.lk"
start "$TMP/inc_use"
body="$(curl -s -m 2 "localhost:$PORT/")"; stop
[ "$body" = "X" ] && ok "a use line in an included file opens the module" \
    || bad "use in included file: body=[$body] log=[$(grep -i error "$TMP/inc_use/log.txt" | head -1)]"

# 5. a module that does not exist → does not start
mk unknown <<'LK'
use nosuchmodule
route("GET", "/", fn() => response::text("started"))
LK
start "$TMP/unknown"
if [ $up = 0 ] && [ "$rc" = 1 ] && grep -q "Unknown module: 'nosuchmodule'" "$TMP/unknown/log.txt"; then
    ok "use of a module that does not exist: the application does not start"
else bad "unknown module: up=$up rc=${rc:-} log=[$(grep -i error "$TMP/unknown/log.txt" | head -1)]"; stop; fi

# 6. `use timer` → its own message, does not start
mk use_timer <<'LK'
use timer
route("GET", "/", fn() => response::text("started"))
LK
start "$TMP/use_timer"
if [ $up = 0 ] && [ "$rc" = 1 ] && grep -q "'timer' is built in and is not a module" "$TMP/use_timer/log.txt"; then
    ok "use timer: the application does not start and says timer:: is built in"
else bad "use timer: up=$up rc=${rc:-} log=[$(grep -i error "$TMP/use_timer/log.txt" | head -1)]"; stop; fi

[ $fail = 0 ] && echo "PASS: the use rule holds in a web application" || { echo "FAIL: use rule"; exit 1; }
