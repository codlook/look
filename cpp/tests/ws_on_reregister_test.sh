#!/usr/bin/env bash
# ws::on can be called again from inside a callback while messages keep arriving.
#
# ws::on assigned the connection's callback (a std::function) on a worker thread while the event
# loop copied it for the next message, with no lock between them: a callback that re-registered
# itself under a message flood crashed the server (ASan: heap-use-after-free copying the callback)
# or silently lost messages (11,153 of 160,000 echoed, then SIGSEGV). The callback slots of a
# WebSocket (message, close) and of an SSE connection (close) are now set and read under a lock.
# Usage: ws_on_reregister_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7754}"; N="${N:-10000}"; C="${C:-8}"
cat > "$TMP/app.lk" <<'LK'
function reg($ws) {
    ws::on($ws, "message", function($m) {
        reg($ws)
        ws::send($ws, $m)
    })
}
route("WS", "/ws", function($ws) { reg($ws) })
route("GET", "/ok", fn() => response::text("alive"))
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, sys, threading
port, N, C = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
def frame(p):
    m = b"\x01\x02\x03\x04"; return bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
got = [0] * C
def conn(k):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=20)
        s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
        d = b""
        while b"\r\n\r\n" not in d: d += s.recv(4096)
        buf = d.split(b"\r\n\r\n", 1)[1]
        sent = 0
        while got[k] < N:
            while sent < N and sent - got[k] < 100:
                s.sendall(b"".join(frame(b"m%d" % (sent + i)) for i in range(100))); sent += 100
            c = s.recv(65536)
            if not c: break
            buf += c
            while len(buf) >= 2 and len(buf) >= 2 + (buf[1] & 0x7f):
                n = buf[1] & 0x7f; buf = buf[2 + n:]; got[k] += 1
    except Exception:
        pass
ts = [threading.Thread(target=conn, args=(k,)) for k in range(C)]
[t.start() for t in ts]; [t.join() for t in ts]
print(sum(got), N * C)
PY
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
read -r got want <<< "$(timeout 120 python3 "$TMP/client.py" "$PORT" "$N" "$C")"
fail=0
[ "${got:-0}" = "$want" ] && echo "  OK   $want messages echoed while each callback re-registered itself" \
    || { echo "  FAIL echoed ${got:-?} of ${want:-?}"; fail=1; }
kill -0 "$pid" 2>/dev/null && echo "  OK   the server is still running" || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: ws::on from inside a callback is safe" || { echo "FAIL: ws::on re-registration"; exit 1; }
