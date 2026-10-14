#!/usr/bin/env bash
# Open connections do not make other clients wait.
#
# Up to 1.0.19 the default dispatch gave each open connection a worker until it sent a request
# or timed out (30 s). As many open connections as there are workers made every other client
# wait: connections that sent nothing, one byte, half a request, or one request and then stayed
# open like a browser does. Measured on the published 1.0.19 with 8 workers: a request from
# another address got no answer for 12 s in all five cases; with 8 idle browser-like
# connections it was answered after 30.5 s.
# From 1.0.20 fibers are the default on Linux; LOOK_FIBER_DISPATCH=0 selects the old model,
# which logs a warning at start when no reverse proxy is configured.
# Usage: idle_connections_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7812}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
echo 'route("GET", "/ok", fn() => response::text("alive"))' > "$TMP/app.lk"
cat > "$TMP/hold.py" <<'PY'
import socket, sys, time
port, workers = int(sys.argv[1]), int(sys.argv[2])
def probe():
    t = time.time()
    try:
        s = socket.socket(); s.settimeout(6); s.bind(("127.0.0.2", 0)); s.connect(("127.0.0.1", port))
        s.sendall(b"GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"); d = s.recv(4096); s.close()
        return "%.2f" % (time.time() - t) if b"alive" in d else "no-body"
    except Exception: return "none"
for name, first in [("nothing", b""), ("one-byte", b"G"), ("half-request", b"GET /ok HTTP/1.1\r\nHost: x\r\n"), ("keep-alive", b"GET /ok HTTP/1.1\r\nHost: x\r\n\r\n")]:
    held = []
    for i in range(workers * 2):
        s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", port))
        if first: s.sendall(first)
        if first.endswith(b"\r\n\r\n"):
            try: s.recv(4096)
            except Exception: pass
        held.append(s)
    time.sleep(0.5); r = probe()
    for s in held: s.close()
    time.sleep(0.5)
    print("%s=%s" % (name, r))
PY
fail=0
run() { ( cd "$TMP" && exec env "$@" "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done; }
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }
if [ "$(uname -s)" = Linux ]; then
    run LOOK_X=1
    out="$(python3 "$TMP/hold.py" "$PORT" 2 2>&1)"
    warned="$(grep -c 'Each open connection holds a worker' "$TMP/log.txt")"
    stop
    for c in nothing one-byte half-request keep-alive; do
        v="$(echo "$out" | sed -n "s/^$c=//p")"
        if [ -n "$v" ] && [ "$v" != none ] && [ "$v" != no-body ] && awk "BEGIN{exit !($v < 2)}"; then echo "  OK   default: 4 connections ($c) held open on 2 workers, another client answered in ${v}s"
        else echo "  FAIL default: 4 connections ($c) held open on 2 workers, another client got: ${v:-nothing}"; fail=1; fi
    done
    if [ "$warned" = 0 ]; then echo "  OK   default: no worker-per-connection warning"; else echo "  FAIL default: the warning is logged although fibers are on"; fail=1; fi
fi
run LOOK_FIBER_DISPATCH=0
warned="$(grep -c 'Each open connection holds a worker' "$TMP/log.txt")"
stop
run LOOK_FIBER_DISPATCH=0 LOOK_TRUSTED_PROXY=127.0.0.1
quiet="$(grep -c 'Each open connection holds a worker' "$TMP/log.txt")"
stop
if [ "$warned" = 1 ] && [ "$quiet" = 0 ]; then echo "  OK   LOOK_FIBER_DISPATCH=0 with no reverse proxy logs the warning; with LOOK_TRUSTED_PROXY it does not"
else echo "  FAIL warning: without proxy=$warned (want 1), with proxy=$quiet (want 0)"; fail=1; fi
[ $fail = 0 ] && echo "PASS: open connections do not make other clients wait" || { echo "FAIL: open connections make other clients wait"; exit 1; }
