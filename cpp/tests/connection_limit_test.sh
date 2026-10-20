#!/usr/bin/env bash
# Open WebSocket / SSE connections: no built-in limit, and no limit of any kind stops the server.
#
# Up to 1.0.14 both had a limit of 1024 open connections, and the connection that went over
# it raised an error nothing caught: the whole server process ended. Anyone who could reach
# the port could stop the application by opening 1025 connections — no login, no valid
# request needed. Measured on the published 1.0.13 and 1.0.14, for WebSocket and for SSE.
#
# What holds in LOOK 2:
#   1. No limit on the total of open connections, and no setting for one. The operating
#      system's open-file limit is the limit, and the server raises its own soft limit to
#      the hard limit at start. (LOOK 1 had LOOK_WS_MAX_CONN / LOOK_SSE_MAX_CONN.)
#   2. One address may hold 1000 open WebSocket / SSE connections by default when the server
#      faces the internet directly (no LOOK_TRUSTED_PROXY); over that the connection gets 429
#      and the address is logged once. With no total limit this is what keeps one source from
#      taking every file descriptor.
#   3. At the open-file limit the server waits: a new connection stays in the listen queue
#      and is taken when a place is free. The process stays up and does not spin.
#   4. LOOK_HTTP_MAX_CONNS_IP, when set, counts every connection of an address, open
#      WebSocket / SSE connections included, and replaces the default of 2.
# Usage: connection_limit_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7758}"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo:" . $m) }) })
route("SSE", "/sse", function($sse) { sse::send($sse, "x") })
route("GET", "/ok", fn() => response::text("alive"))
LK
fail=0
cat > "$TMP/many.py" <<'PY'
import socket, sys, time, os, collections
port, kind, n, close_k = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
SSE = b"GET /sse HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"
SRCS = int(os.environ.get("SRCS", "1")); seq = [0]
def start():
    s = socket.socket(); s.settimeout(3)
    if SRCS > 1: s.bind(("127.0.0.%d" % (1 + seq[0] % SRCS), 0)); seq[0] += 1   # several client addresses
    s.connect(("127.0.0.1", port)); s.sendall(WS if kind == "ws" else SSE); return s
def head(s, t=3):
    s.settimeout(t); h = b""
    try:
        while b"\r\n" not in h:
            d = s.recv(4096)
            if not d: return "closed"
            h += d
    except socket.timeout: return "waiting"
    except OSError: return "reset"
    p = h.split(b"\r\n")[0].split(b" "); return p[1].decode() if len(p) > 1 else "?"
def ok():
    try:
        s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", port)); s.sendall(b"GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        d = s.recv(4096); s.close(); return "alive" if b"alive" in d else "no-body"
    except OSError as e: return "no-answer"
socks, res = [], []
for i in range(n):
    try: s = start()
    except OSError: res.append("connect-fail"); continue
    r = head(s, 3 if not res or res[-1] != "waiting" else 0.2); socks.append((s, r)); res.append(r)
print("opened:", dict(collections.Counter(res)), "| /ok while full:", ok())
if close_k:
    acc = [s for s, r in socks if r in ("101", "200")][:close_k]
    for s in acc: s.close()
    time.sleep(1.5)
    late = collections.Counter(head(s, 1) for s, r in socks if r == "waiting")
    print("after closing %d: waiting ones now:" % len(acc), dict(late), "| /ok:", ok())
if close_k and not any(r == "waiting" for s, r in socks):
    time.sleep(1.0)   # the /ok probe above still holds its per-address place for a moment after its answer
    again = []
    for i in range(close_k + 2):
        try: s = start(); again.append(head(s)); socks.append((s, again[-1]))
        except OSError: again.append("connect-fail")
    print("     %d more after closing %d:" % (close_k + 2, close_k), dict(collections.Counter(again)), "| /ok:", ok())
PY
srv() { ( cd "$TMP" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done; }
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }

# 1. no limit on the total: 1500 connections from three addresses, no setting
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    srv LOOK_X=1
    out="$(SRCS=3 python3 "$TMP/many.py" "$PORT" $kind 1500 0 2>&1 | head -1)"
    stop
    if [ "$out" = "opened: {'$code': 1500} | /ok while full: alive" ]; then echo "  OK   [$kind] 1500 connections open from three addresses with no setting, the server answers"
    else echo "  FAIL [$kind] default: $out $(grep -m1 -o 'terminate called.*' "$TMP/log.txt" | cut -c1-70)"; fail=1; fi
done

# 1b. one address, no setting, no reverse proxy: a default limit of 1000 open WebSocket / SSE
#     connections per address; the connection over it gets 429 and one line in the log.
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    srv LOOK_X=1
    out="$(python3 "$TMP/many.py" "$PORT" $kind 1005 0 2>&1 | head -1)"
    logged="$(grep -c 'refused with 429: the address is at its limit of 1000' "$TMP/log.txt")"
    stop
    if [ "$out" = "opened: {'$code': 1000, '429': 5} | /ok while full: alive" ] && [ "$logged" = 1 ]; then echo "  OK   [$kind] one address: 1000 open, the next 5 answered 429, logged once, ordinary requests still answered"
    else echo "  FAIL [$kind] default per-address limit: $out | log lines: $logged"; fail=1; fi
done

# 1c. behind a reverse proxy every connection comes from the proxy's address: no default limit
srv LOOK_TRUSTED_PROXY=10.9.9.9
out="$(python3 "$TMP/many.py" "$PORT" ws 1100 0 2>&1 | head -1)"
stop
if [ "$out" = "opened: {'101': 1100} | /ok while full: alive" ]; then echo "  OK   [ws] with LOOK_TRUSTED_PROXY set there is no default per-address limit (1100 open from one address)"
else echo "  FAIL [ws] with a trusted proxy: $out"; fail=1; fi

# 1d. the settings LOOK 1 had for a total limit are gone: they change nothing and the log says so
srv LOOK_WS_MAX_CONN=40 LOOK_SSE_MAX_CONN=40
out="$(python3 "$TMP/many.py" "$PORT" ws 60 0 2>&1 | head -1)"
said="$(grep -c 'is set but is not used in LOOK 2' "$TMP/log.txt")"
stop
if [ "$out" = "opened: {'101': 60} | /ok while full: alive" ] && [ "$said" = 2 ]; then echo "  OK   LOOK_WS_MAX_CONN / LOOK_SSE_MAX_CONN are not used: 60 open with both set to 40, and the log says so"
else echo "  FAIL removed settings: $out | log lines: $said"; fail=1; fi

# 2. the open-file limit: wait, do not stop, do not spin
if [ -r /proc/self/stat ]; then
    ( cd "$TMP" && ulimit -Sn 200 && ulimit -Hn 300 && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    t0="$(awk '{print $14+$15}' /proc/$pid/stat)"
    out="$(python3 "$TMP/many.py" "$PORT" ws 400 60 2>&1 | tr '
' ' ')"
    t1="$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)"; up=no; kill -0 "$pid" 2>/dev/null && up=yes
    stop
    # Counter anahtarları ilk görülme sırasında: '101' ilk sırada olmayabilir — {'waiting': 57, '101': 60}
    # (kabul sırası değişince, ör. LOOK_TEST_PREEMPT=1) — sıradan bağımsız oku. ESKİ HATA: sayı okunamıyor,
    # doğru davranan sunucu FAIL alıyordu (fd sızıntısı sanıldı).
    acc="$(echo "$out" | grep -o "opened: {[^}]*}" | grep -o "'101': [0-9]*" | grep -o '[0-9]*$')"
    late="$(echo "$out" | grep -o "waiting ones now: {[^}]*}" | grep -o "'101': [0-9]*" | grep -o '[0-9]*$')"
    if [ "$up" = yes ] && [ "${acc:-0}" -gt 250 ] && [ "${late:-0}" = 60 ] && [ "$((t1 - t0))" -lt 1000 ]; then
        echo "  OK   [open-file limit 300] $acc accepted (soft limit 200 raised), the rest waited, 60 freed places taken by 60 waiting connections, CPU ticks $((t1 - t0))"
    else echo "  FAIL [open-file limit] up=$up accepted=$acc taken-after-freeing=$late cpu-ticks=$((t1 - t0)) | $out"; fail=1; fi
fi

# 3. the per-address limit counts open WebSocket / SSE connections
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    srv LOOK_HTTP_MAX_CONNS_IP=20
    out="$(python3 "$TMP/many.py" "$PORT" $kind 40 5 2>&1 | tr '
' ' ')"
    stop
    case "$out" in
        "opened: {'$code': 20, '429': 20}"*"7 more after closing 5: {'$code': 5, '429': 2}"*) echo "  OK   [$kind] LOOK_HTTP_MAX_CONNS_IP=20: 20 open, the rest 429, 5 freed places taken again" ;;
        *) echo "  FAIL [$kind] per-address limit: $out"; fail=1 ;;
    esac
done

# 3b. a connection under the limit is never refused. The upgrade used to count the connection
# twice for a moment, so an address one below its limit got 429 on its next connection
# (measured: 2966 of 6000 connections inside the limit refused).
cat > "$TMP/pairs.py" <<'PY'
import socket, sys, time, collections
port = int(sys.argv[1])
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
def one():
    s = socket.socket(); s.settimeout(5); s.connect(("127.0.0.1", port)); s.sendall(WS); return s, s.recv(4096).split(b" ")[1].decode()
hold = [one()[0] for i in range(18)]
c = collections.Counter()
for i in range(300):
    a, ra = one(); b, rb = one()   # the 19th and the 20th: both inside the limit
    c[ra] += 1; c[rb] += 1; a.close(); b.close(); time.sleep(0.01)
print(dict(c))
PY
srv LOOK_HTTP_MAX_CONNS_IP=20
out="$(python3 "$TMP/pairs.py" "$PORT" 2>&1 | tail -1)"
stop
if [ "$out" = "{'101': 600}" ]; then echo "  OK   [ws] 600 connections inside the per-address limit, none refused"
else echo "  FAIL [ws] connections inside the per-address limit were refused: $out"; fail=1; fi

[ $fail = 0 ] && echo "PASS: no built-in connection limit; a full limit never stops the server" || { echo "FAIL: connection limit"; exit 1; }
