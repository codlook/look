#!/usr/bin/env bash
# Requests that are suspended in the middle do not see each other's data.
#
# With fibers (the default dispatch on Linux since 1.0.20) one worker thread runs many requests,
# switching between them while one waits — on an outgoing HTTP call, a MySQL query, or a database
# connection. Per-request state that lived in a thread variable would then be read by another
# user's request (v2 had one such variable; it made requests keep database connections).
# Each request here sets and reads back its own path parameter, request header, cookie, session
# value, response status and response header around an HTTP call that suspends it, with 64
# clients at once, and every answer must be its own. After the call a callback (array::map)
# reads the request header too: up to 1.0.23 the callback ran in the VM of whichever request
# had last started on that thread, read that request's header and then crashed the process. Run with fibers and with
# LOOK_FIBER_DISPATCH=0. The fiber run must also show that the requests really overlapped.
# Usage: fiber_request_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null; [ -n "${up:-}" ] && kill "$up" 2>/dev/null' EXIT
PORT="${PORT:-7842}"; UP=$((PORT + 1))
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
cat > "$TMP/app.lk" <<LK
use http
use array
route("GET", "/set/{id}", function(\$id) {
    session::start()
    session::set("who", \$id)
    return response::text("set " . \$id)
})
route("GET", "/t/{id}", function(\$id) {
    session::start()
    \$h0 = request::header("X-Id")
    \$c0 = cookie::get("cid")
    response::status(200 + (\$id % 7))
    response::header("X-Echo", \$id)
    http::get("http://127.0.0.1:$UP/slow")
    \$cb = array::map([1], fn(\$x) => request::header("X-Id"))
    return response::text(\$cb[0] . "|" . \$id . "|" . \$h0 . "|" . request::header("X-Id") . "|" . \$c0 . "|" . cookie::get("cid") . "|" . session::get("who"))
})
route("GET", "/ok", fn() => response::text("alive"))
LK
cat > "$TMP/slow.py" <<'PY'
import http.server, socketserver, time, threading
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(0.03); b = b"slow"
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 1024
S(("127.0.0.1", int(__import__('sys').argv[1])), H).serve_forever()
PY
cat > "$TMP/client.py" <<'PY'
import socket, sys, threading, collections
port, how, n_clients, per = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
res = collections.Counter(); bad = []; lock = threading.Lock()
def req(path, headers):
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n%s\r\n" % (path, "".join("%s: %s\r\n" % kv for kv in headers.items()))).encode())
    d = b""
    while True:
        x = s.recv(65536)
        if not x: break
        d += x
    s.close()
    head, _, body = d.partition(b"\r\n\r\n"); lines = head.decode(errors="replace").split("\r\n")
    hdr = {}
    for l in lines[1:]:
        k, _, v = l.partition(":"); hdr.setdefault(k.strip().lower(), []).append(v.strip())
    return int(lines[0].split(" ")[1]), hdr, body.decode(errors="replace")
def client(c):
    for k in range(per):
        i = c * 1000 + k
        st, hdr, body = req("/set/%d" % i, {})
        sess = [v.split(";")[0] for v in hdr.get("set-cookie", []) if v.lower().startswith("look_session") or "sess" in v.lower().split("=")[0]]
        cookie = "; ".join(sess + ["cid=c%d" % i])
        st, hdr, body = req("/t/%d" % i, {"X-Id": "h%d" % i, "Cookie": cookie})
        want = "h%d|%d|h%d|h%d|c%d|c%d|%d" % (i, i, i, i, i, i, i)
        ok = st == 200 + (i % 7) and hdr.get("x-echo", [""])[0] == str(i) and body == want
        with lock:
            res["ok" if ok else "MIXED"] += 1
            if not ok and len(bad) < 3: bad.append((i, st, hdr.get("x-echo"), body))
ts = [threading.Thread(target=client, args=(c,)) for c in range(n_clients)]
for t in ts: t.start()
for t in ts: t.join()
print("%s: %s %s" % (how, dict(res), bad if bad else ""))
PY
python3 "$TMP/slow.py" "$UP" & up=$!
fail=0
for mode in "LOOK_X=1" "LOOK_FIBER_DISPATCH=0"; do
    ( cd "$TMP" && exec env $mode LOOK_ALLOW_SSRF=1 LOOK_SESSION_SECURE=0 "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    s=$(date +%s%N); out="$(python3 "$TMP/client.py" "$PORT" http 64 30 2>&1 | tail -1)"; ms=$(( ($(date +%s%N) - s) / 1000000 ))
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$out" = "http: {'ok': 1920} " ]; then echo "  OK   [$mode] 1920 requests suspended mid-way, each got its own parameter, header, cookie, session, status and header ($ms ms)"
    else echo "  FAIL [$mode] $out"; fail=1; fi
    # 1920 calls of 30 ms one after another on 4 workers take 14.4 s; overlapping fibers take far less
    if [ "$mode" = "LOOK_X=1" ] && [ "$(uname -s)" = Linux ] && [ $ms -gt 7000 ]; then echo "  FAIL [$mode] the requests did not overlap ($ms ms) — the test did not reach the case it is for"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: suspended requests keep their own data" || { echo "FAIL: request data crossed between suspended requests"; exit 1; }
