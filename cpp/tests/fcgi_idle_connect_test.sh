#!/usr/bin/env bash
# FastCGI TCP mode: a connection that never sends its first request is closed after the idle
# timeout (LOOK_HTTP_IDLE_MS), so silent connections cannot hold every worker.
#
# A worker waited for the first record without a timeout (only later keep-alive requests had
# one): as many silent connections as workers and no request was answered again.
# Usage: fcgi_idle_connect_test.sh <lk-fcgi>   (needs python3)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null; [ -n "${h:-}" ] && kill "$h" 2>/dev/null' EXIT
PORT="${PORT:-7754}"
echo 'route("GET", "/ok", fn() => response::text("ok"))' > "$TMP/app.lk"
cat > "$TMP/c.py" <<'PY'
import socket, struct, sys, time
port, script, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
if mode == "hold":                      # sessiz bağlantılar: hiçbir şey gönderme
    ss = [socket.create_connection(("127.0.0.1", port)) for _ in range(int(sys.argv[4]))]
    time.sleep(60); sys.exit(0)
def rec(t, rid, c): return struct.pack("!BBHHBx", 1, t, rid, len(c), 0) + c
def nv(k, v): return bytes([len(k), len(v)]) + k + v
p = [(b"REQUEST_METHOD", b"GET"), (b"REQUEST_URI", b"/ok"), (b"SCRIPT_NAME", b"/ok"), (b"QUERY_STRING", b""),
     (b"HTTP_HOST", b"x"), (b"REMOTE_ADDR", b"127.0.0.1"), (b"SCRIPT_FILENAME", script.encode())]
s = socket.create_connection(("127.0.0.1", port), timeout=float(sys.argv[4]))
s.sendall(rec(1, 1, struct.pack("!HB5x", 1, 0)) + rec(4, 1, b"".join(nv(k, v) for k, v in p)) + rec(4, 1, b"") + rec(5, 1, b""))
out = b""
try:
    while True:
        hd = s.recv(8, socket.MSG_WAITALL)
        if len(hd) < 8: break
        _, t, _, cl, pl = struct.unpack("!BBHHB", hd[:7])
        body = s.recv(cl + pl, socket.MSG_WAITALL)[:cl] if cl + pl else b""
        if t == 6: out += body
        if t == 3: break
except socket.timeout: print("timeout"); sys.exit(0)
print(out.decode("utf-8", "replace").split("\r\n\r\n", 1)[-1])
PY
fail=0
( cd "$TMP" && exec env LOOK_HTTP_IDLE_MS=2000 "$FCGI" --port "$PORT" --workers 2 --script "$TMP/app.lk" > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" ok 2 2>/dev/null | grep -q ok && break; sleep 0.1; done
python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" hold 4 & h=$!
sleep 0.5
got="$(python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" ok 8 2>&1)"
[ "$got" = ok ] && echo "  OK   with 4 silent connections on 2 workers a request is answered (silent ones closed after 2 s)" \
    || { echo "  FAIL silent connections hold the workers: '$got'"; fail=1; }
[ $fail = 0 ] && echo "PASS: FastCGI silent connections are closed after the idle timeout" \
             || { echo "FAIL: FastCGI first-request timeout"; exit 1; }
