#!/usr/bin/env bash
# http:: does not buffer an unlimited response: a server that never stops sending cannot exhaust
# the process, and gzip responses still decode.
#
# The buffered response had no limit: one http::get to a server that streams forever took the
# LOOK process to 15 GB in 25 s (the machine then kills it, and every site on it). Any feature
# that fetches a URL a user gives (link preview, webhook test, import) reached it. Now a buffered
# response over LOOK_HTTP_MAX_RESPONSE (default 64 MB) is an error; http::stream() is unchanged.
# Usage: http_response_cap_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; kill $pid $up 2>/dev/null' EXIT
PORT="${PORT:-7758}"; UP=$((PORT + 1)); pid=""; up=""
cat > "$TMP/up.py" <<'PY'
import socket, threading, gzip, sys
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1]))); srv.listen(16)
GZ = gzip.compress(b"hello gzip " * 1000)
def h(c):
    req = c.recv(4096)
    if b"/gz" in req:
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(GZ) + GZ); c.close(); return
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n")
    try:
        while True: c.sendall(b"A" * 65536)
    except Exception: pass
while True:
    c, _ = srv.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
PY
cat > "$TMP/app.lk" <<LK
use http
route("GET", "/x", function() { \$r = http::get("http://127.0.0.1:$UP/"); return response::text(strlen(\$r["body"] ?? "") . "|" . (\$r["error"] ?? "")) })
route("GET", "/gz", function() { \$r = http::get("http://127.0.0.1:$UP/gz"); return response::text(strlen(\$r["body"] ?? "") . "|" . (\$r["error"] ?? "")) })
LK
python3 "$TMP/up.py" "$UP" & up=$!
for i in $(seq 1 50); do (echo > /dev/tcp/127.0.0.1/$UP) 2>/dev/null && break; sleep 0.1; done
( cd "$TMP" && exec env LOOK_ALLOW_SSRF=1 LOOK_HTTP_MAX_RESPONSE=1048576 "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/gz" && break; sleep 0.1; done
fail=0
g="$(curl -s -m 20 "localhost:$PORT/gz")"
[ "$g" = "11000|" ] && echo "  OK   a gzip response still decodes" || { echo "  FAIL gzip response: '$g'"; fail=1; }
x="$(curl -s -m 30 "localhost:$PORT/x")"
case "$x" in "0|http:: response exceeded 1048576 bytes"*) echo "  OK   an endless response stops at the limit with an error";;
  *) echo "  FAIL endless response: '$x'"; fail=1;; esac
rss=$(awk '/VmRSS/{print int($2/1024)}' "/proc/$pid/status" 2>/dev/null)
[ -n "$rss" ] && [ "$rss" -lt 200 ] && echo "  OK   the process stayed at ${rss} MB" || { echo "  FAIL process memory ${rss:-?} MB"; fail=1; }
[ $fail = 0 ] && echo "PASS: http:: buffers at most LOOK_HTTP_MAX_RESPONSE bytes" || { echo "FAIL: http:: response limit"; exit 1; }
