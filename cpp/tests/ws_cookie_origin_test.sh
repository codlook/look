#!/usr/bin/env bash
# A WebSocket or SSE route sees the request's headers and cookies, so a session can authenticate
# it; a page on another site still cannot open a WebSocket with the visitor's cookies.
#
# The context of a WS / SSE route had no headers and no cookies: request::header() and
# session::get() returned null there and in the ws::on callbacks, so a WebSocket could not be
# authenticated with the session cookie (it failed silently, as an anonymous visitor).
# With cookies, cross-site WebSocket hijacking matters: browsers apply no CORS to WebSockets. The
# handshake check stays as it was — an Origin of another site (or "null") is refused with 403
# before the route runs; a handshake without Origin (not a browser) is accepted.
# Usage: ws_cookie_origin_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7743}"
cat > "$TMP/app.lk" <<'LK'
route("GET", "/login", function() { session::start(); session::set("user", "alice"); return response::text("in") })
route("WS", "/ws", function($ws) {
    ws::send($ws, "route:" . (session::get("user") ?? "none") . ":" . (request::header("x-tag") ?? "null"))
    ws::on($ws, "message", function($m) {
        ws::send($ws, "cb:" . (session::get("user") ?? "none") . ":" . (request::header("x-tag") ?? "null"))
    })
})
route("SSE", "/ev", function($sse) {
    sse::send($sse, "sse:" . (session::get("user") ?? "none") . ":" . (request::header("x-tag") ?? "null"))
})
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, struct, sys, urllib.request
port = int(sys.argv[1]); mode = sys.argv[2]
r = urllib.request.urlopen("http://127.0.0.1:%d/login" % port)
cookie = r.headers.get("Set-Cookie").split(";")[0]
def frame(p):
    m = os.urandom(4); return bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
# The first frame often arrives in the same read as the 101 response: keep the bytes after it.
buf = [b""]
def take(s, n):
    while len(buf[0]) < n:
        c = s.recv(4096)
        if not c: return None
        buf[0] += c
    out, buf[0] = buf[0][:n], buf[0][n:]
    return out
def recv(s):
    h = take(s, 2)
    if not h or (h[0] & 0x0f) == 8: return "CLOSED"
    n = h[1] & 0x7f
    if n == 126: n = struct.unpack(">H", take(s, 2))[0]
    out = take(s, n)
    return "CLOSED" if out is None else out.decode()
def ws(origin):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    hdr = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" % port
    hdr += "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nCookie: %s\r\nX-Tag: t-42\r\n" % (base64.b64encode(os.urandom(16)).decode(), cookie)
    if origin: hdr += "Origin: %s\r\n" % origin
    s.sendall((hdr + "\r\n").encode())
    d = b""
    while b"\r\n\r\n" not in d:
        c = s.recv(4096)
        if not c: break
        d += c
    if not d.startswith(b"HTTP/1.1 101"): return "REFUSED " + d.split(b"\r\n")[0].decode()[9:12]
    buf[0] = d.split(b"\r\n\r\n", 1)[1]
    got = [recv(s)]
    s.sendall(frame(b"hi")); got.append(recv(s))
    s.close()
    return " ".join(got)
if mode == "same":   print(ws("http://127.0.0.1:%d" % port))
elif mode == "none": print(ws(None))
elif mode == "evil": print(ws("https://evil.example"))
elif mode == "null": print(ws("null"))
elif mode == "sse":
    req = urllib.request.Request("http://127.0.0.1:%d/ev" % port,
                                 headers={"Cookie": cookie, "X-Tag": "t-42", "Accept": "text/event-stream"})
    with urllib.request.urlopen(req, timeout=5) as r:
        for line in r:
            line = line.decode().strip()
            if line.startswith("data:"): print(line[5:].strip()); break
PY
fail=0
check() { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }
( cd "$TMP" && exec env LOOK_SECRET=ws-test-0123456789abcdef0123456789abcdef LOOK_SESSION_DIR="$TMP/s" \
    "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/login" && break; sleep 0.1; done
check "same-site WebSocket: the route and its callback see the session and the headers" \
      "$(python3 "$TMP/client.py" "$PORT" same 2>&1 | tail -1)" "route:alice:t-42 cb:alice:t-42"
check "WebSocket without Origin (not a browser) sees them too" \
      "$(python3 "$TMP/client.py" "$PORT" none 2>&1 | tail -1)" "route:alice:t-42 cb:alice:t-42"
check "WebSocket from another site is refused at the handshake" \
      "$(python3 "$TMP/client.py" "$PORT" evil 2>&1 | tail -1)" "REFUSED 403"
check "WebSocket from an opaque origin (null) is refused at the handshake" \
      "$(python3 "$TMP/client.py" "$PORT" null 2>&1 | tail -1)" "REFUSED 403"
check "SSE route sees the session and the headers" \
      "$(python3 "$TMP/client.py" "$PORT" sse 2>&1 | tail -1)" "sse:alice:t-42"
if kill -0 "$pid" 2>/dev/null; then echo "  OK   the server is still running"; else echo "  FAIL the server died"; fail=1; fi
[ $fail = 0 ] && echo "PASS: WebSocket/SSE routes see headers and cookies; cross-site WebSockets are refused" \
             || { echo "FAIL: WebSocket cookies / Origin"; grep -vE 'Rate limiting|WITHOUT Secure|Listening on|Script:|bypass' "$TMP/log.txt" | cut -c1-250; exit 1; }
