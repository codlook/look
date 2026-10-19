#!/usr/bin/env bash
# LOOK 2 web runs only on the VM: no tree-walk fallback, and a program the VM cannot run does
# not start.
#
# Up to now an application the VM could not compile — or set up — ran on the tree-walk
# interpreter instead, ~40x slower and silently (only LOOK_VM_STRICT refused it), and a route
# whose VM run hit an unlinked built-in was moved to the interpreter for later requests.
#   A. a program the VM cannot compile: lk-fcgi exits with code 1 and says why; nothing listens
#   B. a runtime error in a route is a 500, the request is not run again, the next request works,
#      and nothing is moved to another engine
#   C. hot reload to a file the VM cannot compile keeps the server up: requests get a 500 that
#      says the application could not be loaded; fixing the file brings it back
#   D. LOOK_BYTECODE=0 is ignored (and the log says so): the application runs on the VM
#   E. a script without routes runs whole on every request, on the VM, with fresh globals
# Usage: vm_only_startup_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7738}"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
start() {   # start <dir> <env...>; sets pid, waits for the port or the process to end
    local dir=$1; shift
    ( cd "$dir" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$dir/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 60); do
        curl -s -o /dev/null -m 1 "localhost:$PORT/" && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 0
}
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }

args="$(seq -s, 1 65535)"   # past the VM's 65534-argument limit: valid LOOK, not compilable
good='route("GET", "/", fn() => response::text("ok"))
route("GET", "/boom", function() { cache::set("runs", (cache::get("runs") ?? 0) + 1, 60); throw "boom" })
route("GET", "/runs", fn() => response::text(cache::get("runs")))'

# A
mkdir -p "$TMP/a"
printf 'function cnt(...$a) { return count($a) }\nroute("GET", "/", fn() => response::text(cnt(%s)))\n' "$args" > "$TMP/a/app.lk"
( cd "$TMP/a" && exec "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/a/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$pid" 2>/dev/null; then bad "A: the server is still running with a program the VM cannot compile"; stop
else
    wait "$pid"; rc=$?; pid=""
    if [ $rc = 1 ] && grep -q "Setup error: .*65534" "$TMP/a/log.txt" && ! grep -qi "tree-walk" "$TMP/a/log.txt"; then
        ok "A: a program the VM cannot compile does not start (exit 1, the reason is printed)"
    else bad "A: exit=$rc log=[$(grep -iE 'error|tree-walk' "$TMP/a/log.txt" | head -2 | tr '\n' ' ')]"; fi
fi

# B
mkdir -p "$TMP/b"; printf '%s\n' "use cache" "$good" > "$TMP/b/app.lk"
start "$TMP/b"
b1="$(curl -s -o /dev/null -m 3 -w '%{http_code}' "localhost:$PORT/boom")"
b2="$(curl -s -m 3 "localhost:$PORT/runs")"
b3="$(curl -s -m 3 -w ' %{http_code}' "localhost:$PORT/")"
stop
if [ "$b1" = 500 ] && [ "$b2" = 1 ] && [ "$b3" = "ok 200" ] && ! grep -qiE "interpreter|tree-walk" "$TMP/b/log.txt"; then
    ok "B: a runtime error is a 500, the handler ran once, the next request works, no engine switch"
else bad "B: boom=$b1 runs=[$b2] next=[$b3] log=[$(grep -iE 'interpreter|tree-walk' "$TMP/b/log.txt" | head -1)]"; fi

# C
mkdir -p "$TMP/c"; printf '%s\n' "use cache" "$good" > "$TMP/c/app.lk"
start "$TMP/c" LOOK_HOT_RELOAD_MS=0
c1="$(curl -s -m 3 "localhost:$PORT/")"
sleep 1.1
printf 'function cnt(...$a) { return count($a) }\nroute("GET", "/", fn() => response::text(cnt(%s)))\n' "$args" > "$TMP/c/app.lk"
c2="$(curl -s -m 5 -w ' %{http_code}' "localhost:$PORT/")"
alive=$(kill -0 "$pid" 2>/dev/null && echo yes || echo no)
sleep 1.1
printf '%s\n' "use cache" "$good" > "$TMP/c/app.lk"
c3="$(curl -s -m 5 -w ' %{http_code}' "localhost:$PORT/")"
stop
if [ "$c1" = ok ] && [ "$alive" = yes ] && echo "$c2" | grep -q "could not be loaded.* 500$" && [ "$c3" = "ok 200" ]; then
    ok "C: a reload the VM cannot compile keeps the server up (500 with a clear message), the fixed file loads again"
else bad "C: before=[$c1] broken=[$c2] alive=$alive fixed=[$c3]"; fi

# D
mkdir -p "$TMP/d"; printf '%s\n' "use cache" "$good" > "$TMP/d/app.lk"
start "$TMP/d" LOOK_BYTECODE=0
d1="$(curl -s -m 3 "localhost:$PORT/")"
stop
if [ "$d1" = ok ] && grep -q "LOOK_BYTECODE=0 is ignored" "$TMP/d/log.txt" && grep -q "^\[BYTECODE\] OK" "$TMP/d/log.txt"; then
    ok "D: LOOK_BYTECODE=0 is ignored with a warning; the application runs on the VM"
else bad "D: body=[$d1] warning=$(grep -c 'is ignored' "$TMP/d/log.txt") vm=$(grep -c '^\[BYTECODE\] OK' "$TMP/d/log.txt")"; fi

# E
mkdir -p "$TMP/e"
printf '$n = 0\n$n = $n + 1\nprint("hit " . $n . " " . request::method())\n' > "$TMP/e/app.lk"
start "$TMP/e"
e1="$(curl -s -m 3 "localhost:$PORT/")"; e2="$(curl -s -m 3 "localhost:$PORT/x")"
stop
if [ "$e1" = "hit 1 GET" ] && [ "$e2" = "hit 1 GET" ] && grep -q "each request runs the whole script on the VM" "$TMP/e/log.txt"; then
    ok "E: a script without routes runs whole on every request, on the VM, with fresh globals"
else bad "E: first=[$e1] second=[$e2]"; fi

[ $fail = 0 ] && echo "PASS: the web server runs only on the VM" || { echo "FAIL: VM-only web server"; exit 1; }
