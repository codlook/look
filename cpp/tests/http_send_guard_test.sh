#!/usr/bin/env bash
# A response is sent whole or the connection is closed, and clients that never read cannot freeze
# the server (--mode http).
#
# The response was written with one send(); a short write counted as success. A slow reader got
# part of an 8 MB body, the connection stayed open, and the next response on it read as the rest
# of the first body. With fibers (the Linux default) the socket also blocked: 8 clients asking for
# a large answer and never reading froze the worker threads — another visitor waited 6.7 s.
# Now the response is written in full (the fiber waits for the socket without holding its thread);
# if it cannot be within the idle time the connection is closed.
# Usage: http_send_guard_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7756}"
cat > "$TMP/app.lk" <<'LK'
use string
route("GET", "/h", fn() => response::text("ok"))
route("GET", "/big", function() { print(string::repeat("A", 8388608)) })
LK
cat > "$TMP/c.py" <<'PY'
import socket, sys, time
port, mode = int(sys.argv[1]), sys.argv[2]
def conn(rcvbuf=None):
    s = socket.socket()
    if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect(("127.0.0.1", port)); return s
def readall(s, t):
    s.settimeout(t); d = b""
    try:
        while True:
            c = s.recv(1 << 20)
            if not c: break
            d += c
    except socket.timeout: pass
    return d
if mode == "partial":
    s = conn(4096); s.sendall(b"GET /big HTTP/1.1\r\nHost: x\r\n\r\n")
    time.sleep(4)
    d = readall(s, 2)
    body = d.partition(b"\r\n\r\n")[2]
    try:
        s.sendall(b"GET /h HTTP/1.1\r\nHost: x\r\n\r\n"); d2 = readall(s, 2)
    except Exception: d2 = b""
    full = len(body) == 8388609
    print("full" if full else "short", "next" if d2.startswith(b"HTTP/1.1") else "closed")
else:
    held = []
    for i in range(8):
        s = conn(4096); s.sendall(b"GET /big HTTP/1.1\r\nHost: x\r\n\r\n"); held.append(s)
    time.sleep(1); worst = 0.0
    for i in range(10):
        t = time.time(); s = conn(); s.settimeout(30)
        s.sendall(b"GET /h HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        try: ok = s.recv(64).startswith(b"HTTP/1.1 200")
        except Exception: ok = False
        worst = max(worst, time.time() - t if ok else 99.0); s.close()
    print("%.2f" % worst)
PY
( cd "$TMP" && exec env LOOK_WORKERS=2 LOOK_HTTP_IDLE_MS=1500 "$FCGI" --mode http --port "$PORT" app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/h" && break; sleep 0.1; done
fail=0
p="$(timeout 60 python3 "$TMP/c.py" "$PORT" partial)"
case "$p" in "full closed"|"full next"|"short closed") echo "  OK   a response is whole or its connection is closed ($p)";;
  *) echo "  FAIL a short response kept its connection: $p"; fail=1;; esac
w="$(timeout 120 python3 "$TMP/c.py" "$PORT" slow)"
awk "BEGIN{exit !(${w:-99} < 1.0)}" && echo "  OK   8 clients that never read: other visitors answered in ${w}s at worst" \
    || { echo "  FAIL with 8 non-reading clients another visitor waited ${w}s"; fail=1; }
kill -0 "$pid" 2>/dev/null && echo "  OK   the server is still running" || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: responses are sent whole; non-reading clients do not freeze the server" \
             || { echo "FAIL: HTTP response sending"; exit 1; }
