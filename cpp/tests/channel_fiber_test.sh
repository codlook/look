#!/usr/bin/env bash
# A request waiting on a channel holds only its own fiber, not the thread (fiber dispatch).
# Before, receive()/send() held the THREAD with cv.wait:
#   - keep-alive requests on the same worker stalled for the whole wait (400 ms receive → /ok 350–400 ms)
#   - two requests on a single worker could not talk over the same unbuffered channel: the sender locked the thread,
#     and the receiver could never run (hang)
# Usage: channel_fiber_test.sh <lk-fcgi> [port]   (needs python3; LOOK_TEST_NO_TIMING=1: sanitizer — no latency limits)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7631}"
TMP="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }

cat > "$TMP/app.lk" <<'LK'
$hub = channel(0)
route("GET", "/ok", fn() => response::text("ok"))
route("GET", "/wait", function() {
    $ch = channel(1)
    timer::after(400, function() use ($ch) { send($ch, "late") })
    return response::text(receive($ch))
})
route("GET", "/send", function() use ($hub) {
    send($hub, "hello")
    return response::text("sent")
})
route("GET", "/recv", function() use ($hub) { return response::text(receive($hub)) })
route("GET", "/closed", function() {
    $ch = channel(1)
    timer::after(100, function() use ($ch) { close($ch) })
    $v = receive($ch)
    return response::text($v == null ? "null" : "value")
})
LK

start() { # <env...>
    ( cd "$TMP" && exec env "$@" LOOK_FIBER_DISPATCH=1 LOOK_SECRET=chan-test-secret-0123456789abcdef \
        "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/srv.log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "http://127.0.0.1:$PORT/ok" && return 0; sleep 0.1; done
    echo "FAIL: server did not start"; cat "$TMP/srv.log"; exit 1
}
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }

cat > "$TMP/m.py" <<'PY'
import http.client, sys, threading, time
port = int(sys.argv[1]); slow = sys.argv[2]
def get(path, c=None):
    c = c or http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    t = time.time(); c.request("GET", path); r = c.getresponse(); body = r.read().decode(); return time.time() - t, body, c
ka = get("/ok")[2]                      # keep-alive connection: on the same (single) worker
out = {}
th = threading.Thread(target=lambda: out.setdefault("w", get(slow)[:2])); th.start()
time.sleep(0.05)
worst = 0.0
t0 = time.time()
while time.time() - t0 < 0.25:
    worst = max(worst, get("/ok", ka)[0])
th.join()
print("%d %s %s" % (worst * 1000, round(out["w"][0] * 1000), out["w"][1]))
PY

echo "== a request waiting on receive() does not hold up the worker (1 worker, keep-alive)"
start
read worst took body < <(timeout 30 python3 "$TMP/m.py" "$PORT" /wait)
check "receive() returns the value sent later ($body, ${took} ms)" "$( [ "$body" = late ] && [ "${took:-0}" -ge 350 ] && echo ok)" "body=$body took=$took"
check "/ok on the same worker answers meanwhile (worst ${worst} ms)" "$( [ -n "$worst" ] && { [ -n "${LOOK_TEST_NO_TIMING:-}" ] || [ "$worst" -lt 100 ]; } && echo ok)" "worst=${worst} ms"

echo "== two requests on one worker talk over an unbuffered channel"
r=$( (curl -s -m 10 "http://127.0.0.1:$PORT/recv" > "$TMP/recv.out" &) ; sleep 0.2; curl -s -m 10 "http://127.0.0.1:$PORT/send"; sleep 0.3; cat "$TMP/recv.out")
check "send and receive both finish ($r)" "$( [ "$r" = "senthello" ] && echo ok)" "got '$r'"

echo "== close() wakes a request waiting in receive()"
b=$(curl -s -m 10 "http://127.0.0.1:$PORT/closed")
check "receive() returns null after close ($b)" "$( [ "$b" = null ] && echo ok)" "got '$b'"
stop

echo "== LOOK_CHANNEL_TIMEOUT_MS: a timed wait does not hold up the worker either"
start LOOK_CHANNEL_TIMEOUT_MS=300
read worst took body < <(timeout 30 python3 "$TMP/m.py" "$PORT" /recv)
check "receive() with nobody sending times out (${took} ms)" "$( [ "${took:-0}" -ge 250 ] && [ "${took:-0}" -lt 2000 ] && echo ok)" "took=$took body=$body"
check "/ok on the same worker answers meanwhile (worst ${worst} ms)" "$( [ -n "$worst" ] && { [ -n "${LOOK_TEST_NO_TIMING:-}" ] || [ "$worst" -lt 100 ]; } && echo ok)" "worst=${worst} ms"
grep -q "channel receive timeout" "$TMP/srv.log"
check "the timeout is reported" "$( [ $? = 0 ] && echo ok)" "$(tail -3 "$TMP/srv.log")"
stop

[ $fail = 0 ] && echo "PASS: channel waits suspend the fiber, not the worker" || echo "FAIL: channel waits in fibers"
exit $fail
