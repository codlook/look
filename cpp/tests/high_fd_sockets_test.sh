#!/usr/bin/env bash
# Sockets numbered above 1023 are safe: database connects and the IMAP idle wait, run while
# the server holds more than a thousand connections, under ASan/UBSan.
#
# Up to 1.0.14 these three used select(); FD_SET on a descriptor above 1023 writes outside
# the fd_set. The answers stayed correct, so only a sanitizer build shows it — this test is
# meant for one (it runs on any build, but can only fail there).
#
# A new socket takes the LOWEST free number. If any number below 1024 is free the socket
# under test may land there and the test would pass without testing anything (that happened
# while measuring this fix: the IMAP connection got descriptor 5). So the test first fills
# the low numbers and FAILS if one is still free.
# Usage: high_fd_sockets_test.sh <lk-fcgi built with -DLOOK_SANITIZE=address,undefined>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7768}"; IPORT=$((PORT + 1))
[ -d /proc/self/fd ] || { echo "  (skipped: needs /proc)"; exit 0; }
ulimit -n 8192 2>/dev/null   # the client holds 1100 sockets itself
mkdir -p "$TMP/md"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo:" . $m) }) })
route("GET", "/ok", fn() => response::text("alive"))
route("GET", "/my", function() { try { db::connect("mysql://u:p@127.0.0.1:1/x") } catch ($e) { return response::text("refused") } return response::text("connected") })
route("GET", "/pg", function() { try { db::connect("postgres://u:p@127.0.0.1:1/x") } catch ($e) { return response::text("refused") } return response::text("connected") })
LK
cat > "$TMP/client.py" <<'PY'
import socket, sys, time, os
port, iport, pid = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
hold = []
for i in range(1100):
    c = socket.socket(); c.settimeout(10); c.connect(("127.0.0.1", port)); c.sendall(WS); c.recv(4096); hold.append(c)
def imap():
    s = socket.create_connection(("127.0.0.1", iport), timeout=10); s.recv(4096); return s
hold += [imap() for i in range(8)]
time.sleep(0.5)
free = [n for n in range(3, 1024) if not os.path.exists("/proc/%s/fd/%d" % (pid, n))]
print("free-below-1024=%d" % len(free))
def get(path):
    s = socket.create_connection(("127.0.0.1", port), timeout=20); s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path).encode())
    d = b""
    while True:
        x = s.recv(4096)
        if not x: break
        d += x
    return d.split(b"\r\n\r\n", 1)[-1].decode()
print("my=%s pg=%s" % (get("/my"), get("/pg")))
a = imap(); a.sendall(b"a1 LOGIN u@localhost s123\r\n"); r = a.recv(4096)
a.sendall(b"a2 SELECT INBOX\r\n"); time.sleep(0.3); a.recv(65536)
a.sendall(b"a3 IDLE\r\n"); a.recv(4096); time.sleep(1.2); a.sendall(b"DONE\r\n"); a.settimeout(8)
d = b""
try:
    while b"a3 " not in d:
        x = a.recv(4096)
        if not x: break
        d += x
except socket.timeout: pass
print("imap-login=%s imap-idle=%s" % ("ok" if b"a1 OK" in r else "failed", "ok" if b"a3 OK" in d else "failed"))
print("ok=%s" % get("/ok"))
PY
( cd "$TMP" && exec env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 LOOK_IMAP_PORT=$IPORT LOOK_MAIL_DIR="$TMP/md" LOOK_MAIL_USER="u@localhost" LOOK_MAIL_PASS="s123" LOOK_IMAP_IDLE_TICK_MS=300 "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 120); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.25; done
out="$(python3 "$TMP/client.py" "$PORT" "$IPORT" "$pid" 2>&1 | tr '\n' ' ')"
reports="$(grep -c 'runtime error:\|ERROR: AddressSanitizer' "$TMP/log.txt")"
want="free-below-1024=0 my=refused pg=refused imap-login=ok imap-idle=ok ok=alive "
fail=0
case "$out" in
    "free-below-1024=0 "*) echo "  OK   every descriptor below 1024 is taken: the sockets under test are above 1023" ;;
    *) echo "  FAIL a descriptor below 1024 was free — the test would not reach the high-descriptor path: $out"; fail=1 ;;
esac
if [ "$out" = "$want" ]; then echo "  OK   MySQL connect, PostgreSQL connect and IMAP idle work with 1100 connections held"
else echo "  FAIL $out"; fail=1; fi
if [ "$reports" = 0 ]; then echo "  OK   no sanitizer report"
else echo "  FAIL $reports sanitizer report(s): $(grep -m3 -o 'src/[a-z_]*\.cpp:[0-9]*:[0-9]*: runtime error: [a-z0-9 ]*' "$TMP/log.txt" | tr '\n' ';')"; fail=1; fi
[ "$reports" = 0 ] || grep -E -m1 -A14 'runtime error:|ERROR: AddressSanitizer' "$TMP/log.txt" | cut -c1-220
[ $fail = 0 ] && echo "PASS: sockets above 1023 are safe" || { echo "FAIL: high descriptor sockets"; exit 1; }
