#!/usr/bin/env bash
# Request smuggling through Transfer-Encoding, in both dispatch modes (worker pool and fiber). The request
# is parsed before any engine runs, and since 1.0.8 web requests run on the VM only (a VM error is a 500,
# never a second run on the tree-walk interpreter — tests/vm_fallback_test.sh), so there is one engine here:
#   * two Transfer-Encoding lines are one list ("chunked, chunked") and are refused — they were read as a
#     single chunked body while a front-end that joins the lines refuses or frames them differently;
#   * an HTTP/1.0 request with Transfer-Encoding is answered with 400 (RFC 9112 §6.1: faulty framing) — it was decoded
#     as chunked, while Go ignores the header and reads no body;
#   * after a refused request the connection is closed: a second request sent behind it gets no answer.
# A valid chunked request and a valid HTTP/1.0 request with Content-Length still work (positive control).
# Usage: te_smuggling_test.sh <lk-fcgi> [port]
set -u
LKFCGI="$(cd "$(dirname "${1:?usage: te_smuggling_test.sh <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7583}"
T="$(mktemp -d)"; P=""; trap '[ -n "$P" ] && kill $P 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("POST", "/e", function() { return response::text("B[" . request::body() . "]") })
route("GET", "/sentinel", function() { return response::text("SENTINEL") })
LK
cat > "$T/check.py" <<'PY'
import re, socket, sys
port = int(sys.argv[1])
SENT = b"GET /sentinel HTTP/1.1\r\nHost: s\r\n\r\n"
TE = b"Transfer-Encoding: chunked\r\n"
def send(data):
    s = socket.create_connection(("127.0.0.1", port), timeout=4)
    s.sendall(data)
    try: s.shutdown(socket.SHUT_WR)
    except OSError: pass
    out = b""
    try:
        while True:
            c = s.recv(65536)
            if not c: break
            out += c
    except OSError: pass
    s.close()
    return [c.decode() for c in re.findall(rb"HTTP/1\.[01] (\d{3})", out)], out
cases = [
    ("two TE lines are refused",      b"POST /e HTTP/1.1\r\nHost: x\r\n" + TE + TE + b"\r\n3\r\nabc\r\n0\r\n\r\n" + SENT, ["400"], False),
    ("HTTP/1.0 with TE is refused",   b"POST /e HTTP/1.0\r\n" + TE + b"\r\n3\r\nabc\r\n0\r\n\r\n" + SENT, ["400"], False),
    ("valid chunked request",         b"POST /e HTTP/1.1\r\nHost: x\r\n" + TE + b"\r\n3\r\nabc\r\n0\r\n\r\n", ["200"], True),
    ("valid HTTP/1.0 Content-Length", b"POST /e HTTP/1.0\r\nContent-Length: 3\r\n\r\nabc", ["200"], True),
]
fail = 0
for name, data, want, body in cases:
    codes, out = send(data)
    ok = codes == want and (b"B[abc]" in out) == body and b"SENTINEL" not in out
    print("  %-32s %-6s %s" % (name, "|".join(codes) or "closed", "OK" if ok else "FAIL %r" % out[-80:]))
    fail |= not ok
sys.exit(fail)
PY
rc=0
for mode in pool fiber; do
        envs=(LOOK_FIBER_DISPATCH=0)
        [ $mode = fiber ] && envs=(LOOK_FIBER_DISPATCH=1)
        (cd "$T" && exec env "${envs[@]}" "$LKFCGI" --mode http --port "$PORT" app.lk) > "$T/srv.log" 2>&1 &
        P=$!
        for i in $(seq 1 100); do
            python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" 2>/dev/null && break
            sleep 0.1
        done
        echo "== $mode"
        python3 "$T/check.py" "$PORT" || rc=1
        kill $P 2>/dev/null; wait $P 2>/dev/null; P=""
done
[ $rc = 0 ] && echo "PASS: Transfer-Encoding smuggling shapes are refused (pool and fiber)" || echo "FAIL: Transfer-Encoding smuggling"
exit $rc
