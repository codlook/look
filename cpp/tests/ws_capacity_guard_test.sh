#!/usr/bin/env bash
# Capacity does not regress silently: memory per open WebSocket and the cost of opening one.
#
# Two regressions were found only by re-running a 700,000-connection benchmark by hand:
#   * 1.0.34 kept a full copy of the request context for every ws::on connection — memory per
#     open WebSocket went from 0.86 KB to 1.68 KB, and no test noticed;
#   * with fibers as the default dispatch, every connection mapped and unmapped an 8 MB stack —
#     connections were accepted at a third of the rate of the worker-pool dispatch.
# This test opens WebSockets from many loopback addresses (no per-address limit is reached):
#   * marginal memory: server RSS growth from WARM to WARM+N open connections, per connection
#     (fixed costs such as the stack cache are taken in the warm-up and do not count);
#   * page faults per connection with fibers (the default): a stack mapped anew for every
#     connection costs ~3.3, a reused one ~0.3. The accept rate itself is printed but not checked:
#     on a small runner the client, not the server, sets it, and the slow build passed.
# Limits: MAX_KB per connection (now ~1.2 KB; 1.64 KB before the fix) and MAX_PF page faults.
# Usage: ws_capacity_guard_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7745}"; WARM="${WARM:-4000}"; N="${N:-16000}"; MAX_KB="${MAX_KB:-1.45}"; MAX_PF="${MAX_PF:-1.5}"
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo:" . $m) }) })
route("GET", "/n", fn() => response::text(ws::clients()))
LK
cat > "$TMP/open.py" <<'PY'
# open.py <port> <pid> <warm> <n> : open warm, read RSS, open n more, read RSS; time the n
import socket, selectors, time, sys, base64
port, pid, warm, n = (int(a) for a in sys.argv[1:5])
REQ = (b"GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       b"Sec-WebSocket-Key: " + base64.b64encode(b"0123456789abcdef") + b"\r\nSec-WebSocket-Version: 13\r\n\r\n")
keep = []; made = [0]
def addr(i):
    k = i // 800; return "127.%d.%d.%d" % (10 + k // 62500, (k // 250) % 250 + 1, k % 250 + 1)
def open_many(count):
    sel = selectors.DefaultSelector(); inflight = 0; need = count; fails = 0
    while need or inflight:
        while need and inflight < 1000:
            s = socket.socket(); s.setblocking(False)
            s.bind((addr(made[0]), 0)); made[0] += 1
            try: s.connect(("127.0.0.1", port))
            except BlockingIOError: pass
            sel.register(s, selectors.EVENT_WRITE, [b""]); inflight += 1; need -= 1
        for key, ev in sel.select(5):
            s = key.fileobj
            if ev & selectors.EVENT_WRITE:
                try: s.send(REQ); sel.modify(s, selectors.EVENT_READ, key.data)
                except OSError: sel.unregister(s); s.close(); inflight -= 1; fails += 1
                continue
            try: d = s.recv(4096)
            except OSError: d = b""
            key.data[0] += d
            if not d or b"\r\n\r\n" in key.data[0]:
                sel.unregister(s); inflight -= 1
                if d and key.data[0].startswith(b"HTTP/1.1 101"): keep.append(s)
                else: s.close(); fails += 1
    return fails
def minflt():
    return int(open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()[7])
def rss():
    for l in open("/proc/%d/status" % pid):
        if l.startswith("VmRSS"): return int(l.split()[1])
f1 = open_many(warm); time.sleep(1); r1 = rss(); m1 = minflt()
t0 = time.time(); f2 = open_many(n); dt = time.time() - t0
time.sleep(1); r2 = rss(); m2 = minflt()
print("%d %d %.4f %d %.2f" % (len(keep), f1 + f2, (r2 - r1) / n, int(n / dt), (m2 - m1) / n))
PY
run() {   # env... -> "open fails kb rate"
    ( cd "$TMP" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/n" && break; sleep 0.1; done
    python3 "$TMP/open.py" "$PORT" "$pid" "$WARM" "$N"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; sleep 1
}
fail=0
read -r open_f fails_f kb_f rate_f pf_f <<< "$(run LOOK_FIBER_DISPATCH=1)"
read -r open_p fails_p kb_p rate_p pf_p <<< "$(run LOOK_FIBER_DISPATCH=0)"
want=$((WARM + N))
echo "  fibers: $open_f open, $fails_f failed, ${kb_f} KB per connection, $rate_f connections/s, $pf_f page faults per connection"
echo "  pool:   $open_p open, $fails_p failed, ${kb_p} KB per connection, $rate_p connections/s, $pf_p page faults per connection"
[ "$open_f" = "$want" ] && [ "$open_p" = "$want" ] && echo "  OK   all $want connections opened on both dispatches" \
    || { echo "  FAIL not every connection opened (want $want)"; fail=1; }
awk "BEGIN{exit !($kb_f <= $MAX_KB)}" && echo "  OK   memory per open WebSocket ${kb_f} KB <= $MAX_KB KB" \
    || { echo "  FAIL memory per open WebSocket ${kb_f} KB > $MAX_KB KB"; fail=1; }
awk "BEGIN{exit !($pf_f <= $MAX_PF)}" && echo "  OK   a fiber connection costs ${pf_f} page faults <= $MAX_PF (its stack is reused)" \
    || { echo "  FAIL a fiber connection costs ${pf_f} page faults > $MAX_PF: fiber stacks are mapped anew"; fail=1; }
[ $fail = 0 ] && echo "PASS: WebSocket capacity (memory per connection, fiber stack reuse) has not regressed" \
             || { echo "FAIL: WebSocket capacity regressed"; exit 1; }
