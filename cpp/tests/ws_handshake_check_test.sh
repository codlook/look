#!/usr/bin/env bash
# WebSocket handshake checks: the same-origin rule compares IPv6 hosts correctly and ignores
# letter case; a handshake without a valid Sec-WebSocket-Key (or not a GET) is refused.
#
#   - The port was cut at the last ':' of the host, so the IPv6 hosts "[::1]" and "[::2]" both
#     became "[:" and a page on one was "same origin" with a server on the other.
#   - Host names are case-insensitive: Origin "http://Example.COM" and Host "example.com" are the
#     same site (it was refused).
#   - A handshake with no Sec-WebSocket-Key, or one that is not 16 bytes of base64, got 101 (RFC
#     6455 requires 400); so did a POST.
# Usage: ws_handshake_check_test.sh <lk-fcgi>   (needs python3)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
PORT="${PORT:-7772}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, $m) }) })
route("GET", "/", fn() => response::text("ok"))
LK
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.1; done

python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1]); fail = 0
KEY = "dGhlIHNhbXBsZSBub25jZQ=="
def status(host, origin=None, key=KEY, method="GET"):
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(3)
    h = "%s /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n" % (method, host)
    if key is not None: h += "Sec-WebSocket-Key: %s\r\n" % key
    if origin: h += "Origin: %s\r\n" % origin
    s.sendall((h + "\r\n").encode())
    try: line = s.recv(4096).split(b"\r\n")[0].decode()
    except Exception: line = ""
    s.close()
    return line.split(" ")[1] if " " in line else "none"
def check(name, got, want):
    global fail
    print(("  OK   " if got == want else "  FAIL ") + "%s: %s (want %s)" % (name, got, want))
    if got != want: fail = 1
check("IPv6 page on [::1], server [::2]", status("[::2]", "http://[::1]"), "403")
check("IPv6 page and server on [::1], ports differ", status("[::1]:%d" % port, "http://[::1]:8080"), "101")
check("Origin http://Example.COM, Host example.com", status("example.com", "http://Example.COM"), "101")
check("cross-origin host still refused", status("example.com", "http://evil.example"), "403")
check("no Sec-WebSocket-Key", status("x", key=None), "400")
check("a key that is not 16 bytes of base64", status("x", key="k"), "400")
check("POST upgrade", status("x", method="POST"), "400")
check("a valid handshake", status("x"), "101")
sys.exit(fail)
PY
rc=$?
kill -0 "$pid" 2>/dev/null || { echo "  FAIL the server died"; rc=1; }
[ $rc = 0 ] && echo "PASS: WebSocket handshake: origin hosts compared correctly, invalid handshakes refused" \
           || { echo "FAIL: WebSocket handshake checks"; exit 1; }
