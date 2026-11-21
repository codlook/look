#!/usr/bin/env bash
# The fiber stack cache gives its memory back: after a connection storm, and at once under the memory budget.
#
# Every connection runs on a fiber; a finished fiber's stack is kept for the next connection (accepting is 5x
# faster). Until now its touched pages stayed for good: 1000 connections left 20 MB held after the load, and a
# storm of up to 4096 connections could leave up to 384 MiB that no request used and that the memory budget did
# not see. With a container limit, counted heap + that memory could pass the limit while the budget thought it
# had room — the kernel kills such a process without a word.
#   1) idle: after a storm the held memory falls back within a few seconds (the bottom of the cache, not taken
#      during a 5 s period, is given back);
#   2) budget: LOOK_MEMORY_LIMIT=128MiB. A storm of 1500 connections, each touching ~64 KB of its stack (a
#      60 KB request body), leaves ~100 MB in the cache in the old build. A request then holds 60 MiB of heap: the
#      cache gives way at once (no idle wait), the process stays under its limit and the request is answered.
# Under a sanitizer (LOOK_TEST_NO_TIMING=1) the memory figures mean nothing (shadow memory): the same
# paths run — storm, idle give-back, the budget giving way — and only the answers are checked.
# Usage: fiber_stack_cache_trim_test.sh <lk-fcgi> [port]
set -u
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
P="${2:-7955}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""
trap '{ [ -n "$pid" ] && kill -9 $pid; jobs -p | xargs -r kill; wait; } 2>/dev/null; rm -rf "$T"' EXIT
( sleep 240; echo "FAIL: fiber stack cache test timed out (240 s)"; kill -TERM $$ ) 2>/dev/null &
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)"
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
# a memory figure: checked, or only shown under a sanitizer
mcheck() { if [ "${LOOK_TEST_NO_TIMING:-}" = 1 ]; then echo "  --   $1 (not checked under a sanitizer)"; else check "$@"; fi; }
mkdir -p "$T/sig"
cat > "$T/app.lk" <<LK
use string
use array
use date
use file
\$SIG = "$T/sig/"
LK
cat >> "$T/app.lk" <<'LK'
route("POST", "/p", fn() => response::text("ok"))
// Holds 60 MiB of heap until the test has read the server's memory.
route("GET", "/big", function() {
    $a = []
    for ($i = 0; $i < 60; $i++) { $a = array::push($a, string::repeat("x", 1048576) . $i) }
    $t = date::mono_us()
    while (!file::exists($SIG . "release") && date::mono_us() - $t < 30000000) { $u = date::mono_us(); while (date::mono_us() - $u < 5000) { } }
    return response::text(count($a))
})
LK
cat > "$T/storm.py" <<'PY'
# storm.py <port> <n> <signal-dir>: n keep-alive connections at once, each sends a POST with a 60 KB body and
# reads its answer; with all of them open it writes <dir>/open, waits for <dir>/close, then closes them all.
import socket, selectors, sys, os, time
port, n, sig = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
body = b"b" * 61440
req = (b"POST /p HTTP/1.1\r\nHost: t\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % len(body)) + body
sel = selectors.DefaultSelector(); socks = []; ok = 0
for i in range(n):
    s = socket.socket(); s.setblocking(False)
    try: s.connect(("127.0.0.1", port))
    except BlockingIOError: pass
    socks.append(s); sel.register(s, selectors.EVENT_WRITE, [req, b""])
left = n; t0 = time.time()
while left and time.time() - t0 < 60:
    for key, ev in sel.select(5):
        s, st = key.fileobj, key.data
        if ev & selectors.EVENT_WRITE:
            try:
                k = s.send(st[0]); st[0] = st[0][k:]
                if not st[0]: sel.modify(s, selectors.EVENT_READ, st)
            except (BlockingIOError, InterruptedError): pass
            except OSError: sel.unregister(s); left -= 1
            continue
        try: d = s.recv(4096)
        except (BlockingIOError, InterruptedError): continue
        except OSError: d = b""
        st[1] += d
        if not d or (b"\r\n\r\n" in st[1] and st[1].endswith(b"ok")):
            sel.unregister(s); left -= 1
            if st[1].startswith(b"HTTP/1.1 200"): ok += 1
print(ok)
open(os.path.join(sig, "open"), "w").close()
while not os.path.exists(os.path.join(sig, "close")): time.sleep(0.05)
for s in socks: s.close()
PY
rss() { awk '/VmRSS/{print int($2/1024)}' /proc/$pid/status; }
start() {
    rm -f "$T/sig/"*
    env "$@" "$FCGI" --mode http --port "$P" "$T/app.lk" > "$T/srv.log" 2>&1 &
    pid=$!
    for _ in $(seq 100); do curl -s -o /dev/null -X POST "http://127.0.0.1:$P/p" && return 0; sleep 0.1; done
    echo "  FAIL the server did not start"; cat "$T/srv.log"; exit 1
}
# storm <n>: prints "<answered> <rss with all open, MB> <rss right after they closed, MB>"
storm() {
    python3 "$T/storm.py" "$P" "$1" "$T/sig" > "$T/storm.out" &
    local sp=$!
    for _ in $(seq 600); do [ -f "$T/sig/open" ] && break; sleep 0.1; done
    local live; live=$(rss)
    touch "$T/sig/close"; wait $sp
    sleep 1   # the connections end, their stacks go to the cache
    echo "$(cat "$T/storm.out") $live $(rss)"
}

echo "== 1) idle: the cache gives its memory back after a storm"
start LOOK_MEMORY_LIMIT=off
base=$(rss)
read -r answered live after < <(storm 1500)
check "1500 connections answered" "$([ "$answered" = 1500 ] && echo ok)" "$answered answered"
held=$((after - base))
mcheck "the storm left stack memory in the cache (${held} MB, live ${live} MB)" "$([ "$held" -ge 40 ] && echo ok)" \
    "expected at least 40 MB (each connection touches ~64 KB) — the test no longer measures the cache"
idle=$after
for _ in $(seq 24); do sleep 0.5; idle=$(rss); [ $((idle - base)) -le $((held / 5)) ] && break; done
mcheck "within 12 s at most a fifth of it remains (base ${base} MB, after ${after} MB, idle ${idle} MB)" \
    "$([ $((idle - base)) -le $((held / 5)) ] && echo ok)" "still $((idle - base)) MB above the base"
kill $pid; wait $pid 2>/dev/null; pid=""

echo "== 2) budget: under LOOK_MEMORY_LIMIT=128MiB the cache gives way to the heap at once"
start LOOK_MEMORY_LIMIT=128MiB
base=$(rss)
read -r answered live after < <(storm 1500)
check "1500 connections answered" "$([ "$answered" = 1500 ] && echo ok)" "$answered answered"
curl -s -o "$T/big.out" -w "%{http_code}" "http://127.0.0.1:$P/big" > "$T/big.code" &
cp=$!
peak=0; for _ in $(seq 40); do sleep 0.1; r=$(rss); [ "$r" -gt "$peak" ] && peak=$r; done
touch "$T/sig/release"; wait $cp
check "the 60 MiB request was answered" "$([ "$(cat "$T/big.code")" = 200 ] && [ "$(cat "$T/big.out")" = 60 ] && echo ok)" \
    "status $(cat "$T/big.code"), body $(head -c 80 "$T/big.out")"
mcheck "the process stayed under its limit while it ran (base ${base} MB, cache ${after} MB, peak ${peak} MB)" \
    "$([ "$peak" -lt 128 ] && echo ok)" "peak ${peak} MB ≥ 128 MiB"
kill -0 $pid 2>/dev/null; check "the server is alive" "$([ $? = 0 ] && echo ok)" "it exited"
kill $pid; wait $pid 2>/dev/null; pid=""

[ $fail = 0 ] && echo "fiber stack cache: all OK" || { echo "fiber stack cache: FAIL"; cat "$T/srv.log" | tail -20; }
exit $fail
