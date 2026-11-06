#!/usr/bin/env bash
# One WebSocket client cannot grow the server's memory without limit or stall other WebSockets.
#
# Two ways it could, both measured before the fix:
#   * a frame declaring more than 16 MB (e.g. 2^40) was "need more data", not an error: every byte
#     the client streamed was buffered, and the whole buffer was copied for every frame — 48 MB
#     sent, +45 MB RSS, the single event loop at 100% and another client's echo took 1.7 s;
#   * a flood of tiny frames to a handler that sends nothing back: the connection's inbox had no
#     limit — +1 GB RSS in 10 s and another client's echo took 1.3 s.
# Now an oversized frame closes the connection, and a connection whose inbox passes 1000 messages /
# 16 MB stops being read (TCP slows the client) until its callbacks catch up.
# The memory limit is not meaningful under ASan (its quarantine keeps freed memory resident).
# Usage: ws_dos_guard_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7755}"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/sink", function($ws) {
    ws::on($ws, "message", function($m) {
        $n = 0
        for ($i = 0; $i < 200; $i++) { $n = $n + $i }
    })
})
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo") }) })
route("GET", "/ok", fn() => response::text("ok"))
LK
cat > "$TMP/c.py" <<'PY'
import socket, base64, os, struct, sys, time
port, pid, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def rss():
    for l in open("/proc/%s/status" % pid):
        if l.startswith("VmRSS"): return int(l.split()[1]) // 1024
def hs(path):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, base64.b64encode(os.urandom(16)).decode())).encode())
    d = b""
    while b"\r\n\r\n" not in d: d += s.recv(1)
    return s
def frame(p): return bytes([0x81, 0x80 | len(p)]) + b"\0\0\0\0" + p
def echo_time(v):
    t = time.time(); v.sendall(frame(b"hi")); v.settimeout(30); v.recv(64); return time.time() - t
v = hs("/ws"); r0 = rss(); worst = 0.0
a = hs("/sink" if mode == "flood" else "/ws")
if mode == "big":
    a.sendall(bytes([0x82, 0xFF]) + struct.pack(">Q", 1 << 40) + b"\0\0\0\0")
    sent = 0; t0 = time.time(); closed = False
    try:
        chunk = b"\0" * (1 << 20)
        while time.time() - t0 < 6 and sent < 48:
            a.sendall(chunk); sent += 1
            worst = max(worst, echo_time(v))
    except OSError:
        closed = True
    print("closed=%d sent_mb=%d rss_growth=%d worst_echo=%.3f" % (closed, sent, rss() - r0, worst))
else:
    batch = frame(b"x") * 170000; t0 = time.time(); sent = 0; pending = b""; last = 0.0
    a.setblocking(False)
    while time.time() - t0 < 6:
        if not pending: pending = batch
        try:
            n = a.send(pending); sent += n; pending = pending[n:]   # keep frame boundaries
        except BlockingIOError:
            time.sleep(0.01)
        if time.time() - last > 0.5:
            last = time.time(); worst = max(worst, echo_time(v))
    print("closed=0 sent_mb=%d rss_growth=%d worst_echo=%.3f" % (sent >> 20, rss() - r0, worst))
PY
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
fail=0
read -r big <<< "$(timeout 60 python3 "$TMP/c.py" "$PORT" "$pid" big 2>&1 | tail -1)"
eval "$(echo "$big" | tr ' ' '\n' | grep '=' | sed 's/^/b_/')"
[ "${b_closed:-0}" = 1 ] && [ "${b_sent_mb:-99}" -lt 8 ] && echo "  OK   an oversized frame closes the connection (after ${b_sent_mb} MB)" \
    || { echo "  FAIL oversized frame: $big"; fail=1; }
read -r fl <<< "$(timeout 60 python3 "$TMP/c.py" "$PORT" "$pid" flood 2>&1 | tail -1)"
eval "$(echo "$fl" | tr ' ' '\n' | grep '=' | sed 's/^/f_/')"
[ "${f_rss_growth:-9999}" -lt 64 ] && echo "  OK   a tiny-frame flood grew the server by ${f_rss_growth} MB" \
    || { echo "  FAIL tiny-frame flood: $fl"; fail=1; }
awk "BEGIN{exit !(${f_worst_echo:-99} < 0.5 && ${b_worst_echo:-99} < 0.5)}" \
    && echo "  OK   another WebSocket kept answering (worst echo ${f_worst_echo}s / ${b_worst_echo}s)" \
    || { echo "  FAIL another WebSocket waited: flood ${f_worst_echo}s, big frame ${b_worst_echo}s"; fail=1; }
kill -0 "$pid" 2>/dev/null && echo "  OK   the server is still running" || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: one WebSocket client cannot exhaust memory or stall the others" \
             || { echo "FAIL: WebSocket memory / stall guard"; exit 1; }
