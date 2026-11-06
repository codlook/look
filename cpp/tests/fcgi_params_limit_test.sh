#!/usr/bin/env bash
# FastCGI: a request's PARAMS are limited to 2 MB in total (the HTTP mode's header limit).
#
# PARAMS records were collected without a limit: anyone who could reach the FastCGI port could
# send PARAMS records with new names until the process ran out of memory. Over 2 MB the request
# is ended and the connection closed; normal requests are not affected.
# Usage: fcgi_params_limit_test.sh <lk-fcgi>   (needs python3)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7753}"
echo 'route("GET", "/ok", fn() => response::text("ok"))' > "$TMP/app.lk"
cat > "$TMP/c.py" <<'PY'
import socket, struct, sys, os
port, script, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def rec(t, rid, c): return struct.pack("!BBHHBx", 1, t, rid, len(c), 0) + c
def nv(k, v):
    L = lambda n: bytes([n]) if n < 128 else struct.pack("!I", n | 0x80000000)
    return L(len(k)) + L(len(v)) + k + v
base = [(b"REQUEST_METHOD", b"GET"), (b"REQUEST_URI", b"/ok"), (b"SCRIPT_NAME", b"/ok"), (b"QUERY_STRING", b""),
        (b"HTTP_HOST", b"x"), (b"REMOTE_ADDR", b"127.0.0.1"), (b"SCRIPT_FILENAME", script.encode())]
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.sendall(rec(1, 1, struct.pack("!HB5x", 1, 0)) + rec(4, 1, b"".join(nv(k, v) for k, v in base)))
if mode == "flood":
    sent = 0
    try:
        while sent < 200 * 1024 * 1024:   # 200 MB'a kadar yeni adlı PARAMS
            body = b"".join(nv(b"HTTP_X_" + os.urandom(8).hex().encode(), b"v" * 200) for _ in range(250))
            s.sendall(rec(4, 1, body)); sent += len(body)
    except OSError:
        pass
    print("closed-after-MB %d" % (sent // (1024 * 1024)))
else:
    s.sendall(rec(4, 1, b"") + rec(5, 1, b""))
    out = b""
    while True:
        h = s.recv(8, socket.MSG_WAITALL)
        if len(h) < 8: break
        _, t, _, cl, pl = struct.unpack("!BBHHB", h[:7])
        body = s.recv(cl + pl, socket.MSG_WAITALL)[:cl] if cl + pl else b""
        if t == 6: out += body
        if t == 3: break
    print(out.decode("utf-8", "replace").split("\r\n\r\n", 1)[-1])
PY
rss() { awk '/VmRSS/{print int($2/1024)}' "/proc/$pid/status" 2>/dev/null; }
fail=0
( cd "$TMP" && exec "$FCGI" --port "$PORT" --workers 2 --script "$TMP/app.lk" > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" ok 2>/dev/null | grep -q ok && break; sleep 0.1; done
r0=$(rss)
got="$(timeout 120 python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" flood 2>&1)"; r1=$(rss)
mb="${got##* }"
[ -n "$mb" ] && [ "$mb" -lt 10 ] 2>/dev/null && echo "  OK   the connection was closed after ${mb} MB of PARAMS" \
    || { echo "  FAIL PARAMS were accepted without a limit ($got)"; fail=1; }
[ -n "$r1" ] && [ $((r1 - r0)) -lt 20 ] && echo "  OK   memory grew $((r1 - r0)) MB" \
    || { echo "  FAIL memory grew $(( ${r1:-0} - ${r0:-0} )) MB"; fail=1; }
[ "$(python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" ok 2>&1)" = ok ] && echo "  OK   a normal request is still answered" \
    || { echo "  FAIL a normal request after the flood"; fail=1; }
[ $fail = 0 ] && echo "PASS: FastCGI PARAMS are limited" || { echo "FAIL: FastCGI PARAMS limit"; exit 1; }
