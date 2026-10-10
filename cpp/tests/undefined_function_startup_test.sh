#!/usr/bin/env bash
# LOOK 2: an application whose route calls a module function that does not exist does not
# start.
#
# Every module function is provided by the VM (`lk --vm-report`); a `mod::fn` name the VM does
# not know is a spelling mistake. Such a route used to be handed to the tree-walk engine at
# startup with a warning, and the mistake only showed when that line ran — on the first
# request that reached it, in production. Now:
#   * the server refuses to start and names the route and the function (default and strict)
#   * the same for a before_route, which every route passes through
#   * `lk --check` reports it for a core module, exit status 1
#   * an application without the mistake starts
# Usage: undefined_function_startup_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7763}"
fail=0
cat > "$TMP/route.lk" <<'LK'
use string
route("GET", "/ok", fn() => response::text("ok"))
route("GET", "/rare", function() {
    if (request::get("never") == "1") { return response::text(string::uper("x")) }
    return response::text("fine")
})
LK
cat > "$TMP/mw.lk" <<'LK'
before_route(function() { if (request::get("never") == "1") { session::ident() } })
route("GET", "/ok", fn() => response::text("ok"))
LK
cat > "$TMP/good.lk" <<'LK'
use string
route("GET", "/ok", fn() => response::text(string::upper("ok")))
LK
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1"; do
    for app in "route.lk GET:/rare string::uper" "mw.lk GET:/ok session::ident"; do
        set -- $app
        ( cd "$TMP" && exec env $mode timeout 5 "$FCGI" --mode http --port "$PORT" --workers 1 "$1" > "$TMP/log.txt" 2>&1 ); rc=$?
        if [ "$rc" = "1" ] && grep -q "Error: route $2 calls $3(), which does not exist" "$TMP/log.txt"; then
            echo "  OK   [$mode] $1 does not start: the route and $3 are named"
        else echo "  FAIL [$mode] $1: exit=$rc (124 = it started) log: $(grep -m1 -i 'error' "$TMP/log.txt" | cut -c1-140)"; fail=1; fi
    done
done
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 good.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
g="$(curl -s -m 3 "localhost:$PORT/ok")"; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$g" = "OK" ]; then echo "  OK   an application without the mistake starts and answers"; else echo "  FAIL good.lk answered [$g]"; fail=1; fi
out="$("$LK" --check "$TMP/route.lk" 2>/dev/null)"; rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q '^CHECK .*\[undefined-function\] string::uper() does not exist'; then echo "  OK   lk --check reports it and fails"
else echo "  FAIL lk --check: exit=$rc [$out]"; fail=1; fi
out="$("$LK" --check "$TMP/good.lk" 2>/dev/null)"; rc=$?
if [ "$rc" = "0" ] && [ "$out" = "OK" ]; then echo "  OK   lk --check passes the application without the mistake"; else echo "  FAIL lk --check good.lk: exit=$rc [$out]"; fail=1; fi
[ $fail = 0 ] && echo "PASS: a call to a function that does not exist stops the application at startup" || { echo "FAIL: undefined function at startup"; exit 1; }
