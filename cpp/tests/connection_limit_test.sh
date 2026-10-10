#!/usr/bin/env bash
# Open WebSocket / SSE connections: no built-in limit, and no limit of any kind stops the server.
#
# Up to 1.0.14 both had a limit of 1024 open connections, and the connection that went over
# it raised an error nothing caught: the whole server process ended. Anyone who could reach
# the port could stop the application by opening 1025 connections — no login, no valid
# request needed. Measured on the published 1.0.13 and 1.0.14, for WebSocket and for SSE.
#
# What holds now:
#   1. No default limit. The operating system's open-file limit is the limit, and the server
#      raises its own soft limit to the hard limit at start.
#   2. At the open-file limit the server waits: a new connection stays in the listen queue
#      and is taken when a place is free. The process stays up and does not spin (1.0.14
#      burned every core in a retry loop here).
#   3. LOOK_HTTP_MAX_CONNS_IP counts open WebSocket / SSE connections too (it used to let go
#      of them at the upgrade, so one address could hold any number).
#   4. LOOK_WS_MAX_CONN / LOOK_SSE_MAX_CONN, when set, answer 503 over the limit.
# Usage: connection_limit_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7758}"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo:" . $m) }) })
route("SSE", "/sse", function($sse) { sse::send($sse, "x") })
route("GET", "/ok", fn() => response::text("alive"))
LK
cat > "$TMP/client.py" <<'PY'
import socket, sys, time, os
port, kind, limit = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
SSE = b"GET /sse HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"
def open_one():
    s = socket.socket(); s.settimeout(5); s.connect(("127.0.0.1", port)); s.sendall(WS if kind == "ws" else SSE)
    head = b""
    while b"\r\n\r\n" not in head:
        d = s.recv(4096)
        if not d: break
        head += d
    return s, head.split(b"\r\n")[0].decode(errors="replace")
socks, first = [], ""
for i in range(limit):
    s, line = open_one(); socks.append(s)
    if i == 0: first = line
over = []
for i in range(5):
    try: s, line = open_one(); over.append(line.split(" ")[1] if " " in line else "?"); s.close()
    except OSError as e: over.append("error")
# the open ones still work
works = "n/a"
if kind == "ws":
    s = socks[7]; m = os.urandom(4); p = b"ping"
    s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))
    buf = b""
    try:
        while b"echo:ping" not in buf:
            d = s.recv(4096)
            if not d: break
            buf += d
    except OSError: pass
    works = "yes" if b"echo:ping" in buf else "no"
# free two places, take them again
socks[0].close(); socks[1].close(); time.sleep(1.0)
again = []
for i in range(2):
    try: s, line = open_one(); again.append(line.split(" ")[1] if " " in line else "?"); socks.append(s)
    except OSError: again.append("error")
print("accepted=%d first=[%s] over=%s open-still-work=%s after-freeing=%s" % (limit, first.split(" ")[1] if " " in first else first, ",".join(over), works, ",".join(again)))
PY
fail=0
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    ( cd "$TMP" && exec env LOOK_WS_MAX_CONN=40 LOOK_SSE_MAX_CONN=40 "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    out="$(python3 "$TMP/client.py" "$PORT" $kind 40 2>&1 | tail -1)"
    alive="$(curl -s -m 3 "localhost:$PORT/ok")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    want="accepted=40 first=[$code] over=503,503,503,503,503 open-still-work=$([ $kind = ws ] && echo yes || echo n/a) after-freeing=$code,$code"
    if [ "$out" = "$want" ] && [ "$alive" = "alive" ]; then echo "  OK   [$kind] LOOK_*_MAX_CONN=40: 40 accepted, the next 5 answered 503, the server kept answering, freed places taken again"
    else echo "  FAIL [$kind] $out | server answers: [${alive:-nothing}] $(grep -m1 -o 'terminate called.*' "$TMP/log.txt" | cut -c1-70)"; fail=1; fi
done
cat > "$TMP/many.py" <<'PY'
import socket, sys, time, os, collections
port, kind, n, close_k = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
WS = b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
SSE = b"GET /sse HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"
def start():
    s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", port)); s.sendall(WS if kind == "ws" else SSE); return s
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
    again = []
    for i in range(close_k + 2):
        try: s = start(); again.append(head(s)); socks.append((s, again[-1]))
        except OSError: again.append("connect-fail")
    print("     %d more after closing %d:" % (close_k + 2, close_k), dict(collections.Counter(again)), "| /ok:", ok())
PY
srv() { ( cd "$TMP" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done; }
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }

# 1. no default limit
for kind in ws sse; do
    code=101; [ $kind = sse ] && code=200
    srv LOOK_X=1
    out="$(python3 "$TMP/many.py" "$PORT" $kind 1500 0 2>&1 | head -1)"
    stop
    if [ "$out" = "opened: {'$code': 1500} | /ok while full: alive" ]; then echo "  OK   [$kind] 1500 connections open with no setting, the server answers"
    else echo "  FAIL [$kind] default: $out $(grep -m1 -o 'terminate called.*' "$TMP/log.txt" | cut -c1-70)"; fail=1; fi
done

# 2. the open-file limit: wait, do not stop, do not spin
if [ -r /proc/self/stat ]; then
    ( cd "$TMP" && ulimit -Sn 200 && ulimit -Hn 300 && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    t0="$(awk '{print $14+$15}' /proc/$pid/stat)"
    out="$(python3 "$TMP/many.py" "$PORT" ws 400 60 2>&1 | tr '
' ' ')"
    t1="$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)"; up=no; kill -0 "$pid" 2>/dev/null && up=yes
    stop
    acc="$(echo "$out" | grep -o "opened: {'101': [0-9]*" | grep -o '[0-9]*$')"; late="$(echo "$out" | grep -o "waiting ones now: {'101': [0-9]*" | grep -o '[0-9]*$')"
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
[ $fail = 0 ] && echo "PASS: no built-in connection limit; a full limit never stops the server" || { echo "FAIL: connection limit"; exit 1; }
