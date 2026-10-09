#!/usr/bin/env bash
# The WebSocket and SSE connection limits refuse the next connection; they do not stop the
# server.
#
# Both have a limit of open connections (LOOK_WS_MAX_CONN / LOOK_SSE_MAX_CONN, default 1024).
# The connection that went over it raised an error nothing caught: the whole server process
# ended. Anyone who could reach the port could stop the application by opening 1025
# connections — no login, no valid request needed. Measured on the published 1.0.13, for
# WebSocket and for SSE.
# Now the connection over the limit is answered 503 and closed; the open ones keep working,
# ordinary requests are answered, and a place freed by a closed connection can be taken
# again.
# Usage: connection_limit_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7758}"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo:" . $m) }) })
route("SSE", "/sse", function($sse) { sse::send($sse, "x") })
route("GET", "/ok", fn() => response::text("alive"))
LK
cat > "$TMP/client.py" <<'PY'
import socket, sys, time, os
port, kind, limit = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
SSE = b"GET /sse HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"
def open_one():
    s = socket.socket(); s.settimeout(5); s.connect(("127.0.0.1", port)); s.sendall(WS if kind == "ws" else SSE)
    head = b""
    while b"\r\n\r\n" not in head:
        d = s.recv(4096)
        if not d: break
        head += d
    return s, head.split(b"\r\n")[0].decode(errors="replace")
socks, first = [], ""
for i in range(limit):
    s, line = open_one(); socks.append(s)
    if i == 0: first = line
over = []
for i in range(5):
    try: s, line = open_one(); over.append(line.split(" ")[1] if " " in line else "?"); s.close()
    except OSError as e: over.append("error")
# the open ones still work
works = "n/a"
if kind == "ws":
    s = socks[7]; m = os.urandom(4); p = b"ping"
    s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))
    buf = b""
    try:
        while b"echo:ping" not in buf:
            d = s.recv(4096)
            if not d: break
            buf += d
    except OSError: pass
    works = "yes" if b"echo:ping" in buf else "no"
# free two places, take them again
socks[0].close(); socks[1].close(); time.sleep(1.0)
again = []
for i in range(2):
    try: s, line = open_one(); again.append(line.split(" ")[1] if " " in line else "?"); socks.append(s)
    except OSError: again.append("error")
print("accepted=%d first=[%s] over=%s open-still-work=%s after-freeing=%s" % (limit, first.split(" ")[1] if " " in first else first, ",".join(over), works, ",".join(again)))
PY
fail=0
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    ( cd "$TMP" && exec env LOOK_WS_MAX_CONN=40 LOOK_SSE_MAX_CONN=40 "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    out="$(python3 "$TMP/client.py" "$PORT" $kind 40 2>&1 | tail -1)"
    alive="$(curl -s -m 3 "localhost:$PORT/ok")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    want="accepted=40 first=[$code] over=503,503,503,503,503 open-still-work=$([ $kind = ws ] && echo yes || echo n/a) after-freeing=$code,$code"
    if [ "$out" = "$want" ] && [ "$alive" = "alive" ]; then echo "  OK   [$kind] 40 accepted, the next 5 answered 503, the server kept answering, freed places taken again"
    else echo "  FAIL [$kind] $out | server answers: [${alive:-nothing}] $(grep -m1 -o 'terminate called.*' "$TMP/log.txt" | cut -c1-70)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: a full connection limit refuses and the server stays up" || { echo "FAIL: connection limit"; exit 1; }
