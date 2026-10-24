#!/usr/bin/env bash
# A WebSocket callback reads the request of its own handshake; it no longer kills the process.
#
# ws::on stored the callback with an interpreter copy that had no web context: the handshake
# request had ended when a message arrived on another thread. request::get inside a "message"
# or "close" callback touched an invalid context and the whole process died with SIGSEGV —
# in lk-fcgi every site on the binary, from one WebSocket message.
# The callback now sees a copy of the handshake request: request::get returns the ws URL's
# parameters, and requests running at the same time (other users) never show through.
# Usage: ws_callback_context_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7741}"
cat > "$TMP/app.lk" <<'LK'
use cache
route("WS", "/ws", function($ws) {
    ws::on($ws, "message", function($m) { ws::send($ws, "u=" . request::get("u", "")) })
    ws::on($ws, "close", function() { cache::set("closed_u", request::get("u", ""), 60) })
})
route("GET", "/other", fn() => response::text("other " . request::get("u", "")))
route("GET", "/closed", fn() => response::text(cache::get("closed_u") ?? "none"))
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, struct, sys, threading, time, urllib.request
port = int(sys.argv[1])
def frame(p, op=0x81):
    m = os.urandom(4); return bytes([op, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
def recv(s):
    h = s.recv(2)
    if len(h) < 2: return None
    n = h[1] & 0x7f
    if n == 126: n = struct.unpack(">H", s.recv(2))[0]
    out = b""
    while len(out) < n:
        c = s.recv(n - len(out))
        if not c: return None
        out += c
    return out.decode()
s = socket.create_connection(("127.0.0.1", port), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws?u=alice HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
d = b""
while b"\r\n\r\n" not in d: d += s.recv(4096)
def others():
    for i in range(40): urllib.request.urlopen("http://127.0.0.1:%d/other?u=bob%d" % (port, i)).read()
ts = [threading.Thread(target=others) for _ in range(3)]
[t.start() for t in ts]
got = []
for _ in range(6):
    s.sendall(frame(b"hi"))
    try: got.append(recv(s))
    except Exception as e: got.append("ERR " + str(e))
[t.join() for t in ts]
s.close()
print(" ".join(str(g) for g in got))
PY
fail=0
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/closed" && break; sleep 0.1; done
got="$(python3 "$TMP/client.py" "$PORT" 2>&1)"
sleep 0.8
want="u=alice u=alice u=alice u=alice u=alice u=alice"
if [ "$got" = "$want" ]; then echo "  OK   message callback: request::get is the handshake's, 6/6"
else echo "  FAIL message callback: $got"; fail=1; fi
closed="$(curl -s -m 3 "localhost:$PORT/closed")"
if [ "$closed" = "alice" ]; then echo "  OK   close callback: request::get is the handshake's"
else echo "  FAIL close callback: '$closed'"; fail=1; fi
if kill -0 "$pid" 2>/dev/null; then echo "  OK   the server is still running"
else wait "$pid"; echo "  FAIL the server died (exit $?)"; fail=1; fi
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
[ $fail = 0 ] && echo "PASS: WebSocket callbacks see their own handshake request" || { echo "FAIL: WebSocket callback context"; exit 1; }
