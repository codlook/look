#!/usr/bin/env bash
# A Redis session error leaves a trace in the log (LOOK 1 keeps the behaviour).
#
# With LOOK_SESSION_DRIVER=redis, LOOK 1 swallows Redis errors: the session reads as empty and
# session::set loses its data. That stays (running sites must not start failing), but the log
# used to say nothing — users looked logged out and nobody could find why. Now a WARN line names
# the failure, at most one per 10 seconds with a count of the ones in between. And the broken
# connection is dropped: after Redis restarted, a thread kept its dead connection and its
# sessions stayed empty (writes lost) until LOOK itself was restarted.
# Usage: redis_session_warn_test.sh <lk-fcgi>   (needs curl)
set -u
LKF="$1"
PORT=7491; DOWN=7492   # DOWN: nothing listens there
T=$(mktemp -d); trap '{ kill $pid $pid2 $rpid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""; pid2=""; rpid=""
cat > "$T/app.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", "x"); return response::text("ok:" . session::get("v")) })
LK
LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="redis://127.0.0.1:$DOWN" LOOK_SESSION_SECURE=0 \
    "$LKF" --mode http --port $PORT --workers 2 "$T/app.lk" >"$T/srv.log" 2>&1 &
pid=$!
for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
fail=0
codes=""
for i in $(seq 1 20); do codes="$codes$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/set") "; done
sleep 0.3
n=$(grep -c "Redis session storage failed" "$T/srv.log")
if echo "$codes" | grep -qv "^\(200 \)*$"; then echo "FAIL: behaviour changed: statuses $codes"; fail=1
else echo "PASS: requests still answer 200 while Redis is down (behaviour unchanged)"; fi
if [ "$n" -ge 1 ] && [ "$n" -le 2 ]; then
    echo "PASS: the failure is logged ($n warning line(s) for 20 requests): $(grep -m1 'Redis session storage failed' "$T/srv.log" | sed 's/.*\[SESSION\] //')"
else
    echo "FAIL: want 1–2 warning lines for 20 requests within 10 s, got $n"; tail -5 "$T/srv.log"; fail=1
fi

# Redis yeniden başlayınca oturumlar düzelmeli: eskiden iş parçacığının bağlantısı hiç
# yenilenmiyordu, LOOK yeniden başlatılana dek oturum boş, yazılan veri kayıp kalıyordu.
cat > "$T/fake_redis.py" <<'PY'
import socket, sys, threading
store = {}
def rd(f):
    line = f.readline()
    if not line: return None
    n = int(line[1:]); args = []
    for _ in range(n):
        ln = int(f.readline()[1:]); args.append(f.read(ln + 2)[:-2])
    return args
def h(c):
    f = c.makefile("rb")
    while True:
        a = rd(f)
        if a is None: break
        cmd = a[0].upper()
        if cmd == b"GET":
            v = store.get(a[1]); c.sendall(b"$-1\r\n" if v is None else b"$%d\r\n%s\r\n" % (len(v), v))
        elif cmd == b"SET": store[a[1]] = a[2]; c.sendall(b"+OK\r\n")
        elif cmd == b"DEL": store.pop(a[1], None); c.sendall(b":1\r\n")
        else: c.sendall(b"+OK\r\n")
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(16)
while True:
    c, _ = s.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
PY
RP=7493; P2=7494
python3 "$T/fake_redis.py" $RP & rpid=$!
sleep 0.3
cat > "$T/app2.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", request::get("v")); return response::text("ok") })
route("GET", "/get", function() { session::start(); return response::text("v=" . session::get("v")) })
LK
LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="redis://127.0.0.1:$RP" LOOK_SESSION_SECURE=0 \
    "$LKF" --mode http --port $P2 --workers 2 "$T/app2.lk" >"$T/srv2.log" 2>&1 &
pid2=$!
for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P2/health" >/dev/null && break; sleep 0.2; done
q() { curl -s -b "$T/cj" -c "$T/cj" --max-time 5 "http://127.0.0.1:$P2$1"; }
for i in 1 2 3 4; do q "/set?v=one" >/dev/null; q /get >/dev/null; done   # her iş parçacığı bağlansın
kill $rpid; wait $rpid 2>/dev/null
python3 "$T/fake_redis.py" $RP & rpid=$!
sleep 0.3
res=""
for i in 1 2 3 4 5 6; do q "/set?v=two$i" >/dev/null; res="$res $(q /get)"; done
{ kill $rpid $pid2 $pid; wait $rpid $pid2 $pid; } 2>/dev/null
last3=$(echo $res | tr ' ' '\n' | tail -3 | tr '\n' ' ')
if [ "$last3" = "v=two4 v=two5 v=two6 " ]; then
    echo "PASS: sessions work again after Redis restarted (reads after restart:$res)"
else
    echo "FAIL: sessions stay broken after Redis restarted (reads after restart:$res)"; fail=1
fi
exit $fail
