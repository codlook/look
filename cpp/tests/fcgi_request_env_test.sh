#!/usr/bin/env bash
# FastCGI: a request's parameters never become process environment variables.
#
# Every request's parameters — the client's headers as HTTP_* among them — were written into the
# process environment with setenv(), from all worker threads at once:
#   * getenv() (env(), or a multipart upload's TEMP lookup) raced with setenv() and the server died
#     with SIGSEGV (16 threads sending new header names: down after ~3000 requests);
#   * glibc never frees old environment strings: unique header values grew the process for good;
#   * one request's header showed in later requests: env("HTTP_X_SECRET") returned the previous
#     visitor's X-Secret.
# Now request data is read only through request::; env() is configuration (.env + process
# environment). file::store still refuses an upload directory under the request's DOCUMENT_ROOT.
# Usage: fcgi_request_env_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7752}"
cat > "$TMP/app.lk" <<'LK'
route("GET", "/env", function() { print("[" . env("HTTP_X_SECRET") . "][" . (request::header("x-secret") ?? "") . "]") })
route("GET", "/store", function() {
    file::put("up.txt", "hello")
    try { file::store(["__assoc__", "path", "up.txt", "mime", "text/plain"], "u"); print("stored") }
    catch ($e) { print("refused: " . $e) }
})
LK
cat > "$TMP/c.py" <<'PY'
import socket, struct, sys, os, time, threading
port, script, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def rec(t, rid, c): return struct.pack("!BBHHBx", 1, t, rid, len(c), 0) + c
def nv(k, v):
    k = k.encode(); v = v.encode()
    L = lambda n: bytes([n]) if n < 128 else struct.pack("!I", n | 0x80000000)
    return L(len(k)) + L(len(v)) + k + v
def req(uri, extra=()):
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    params = [("REQUEST_METHOD", "GET"), ("REQUEST_URI", uri), ("SCRIPT_NAME", uri), ("QUERY_STRING", ""),
              ("HTTP_HOST", "x"), ("REMOTE_ADDR", "127.0.0.1"), ("SCRIPT_FILENAME", script),
              ("DOCUMENT_ROOT", os.path.dirname(script))] + list(extra)
    s.sendall(rec(1, 1, struct.pack("!HB5x", 1, 0)) + rec(4, 1, b"".join(nv(k, v) for k, v in params)) + rec(4, 1, b"") + rec(5, 1, b""))
    out = b""
    while True:
        h = s.recv(8, socket.MSG_WAITALL)
        if len(h) < 8: break
        _, t, _, cl, pl = struct.unpack("!BBHHB", h[:7])
        body = s.recv(cl + pl, socket.MSG_WAITALL)[:cl] if cl + pl else b""
        if t == 6: out += body
        if t == 3: break
    s.close()
    return out.decode("utf-8", "replace").split("\r\n\r\n", 1)[-1]
if mode == "leak":
    a = req("/env", [("HTTP_X_SECRET", "alice-token-123")])
    b = req("/env")
    print(a.strip() + " " + b.strip())
elif mode == "store":
    print(req("/store"))
elif mode == "race":
    stop = time.time() + float(sys.argv[4]); n = [0]; errs = [0]
    def w():
        while time.time() < stop:
            try: req("/env", [("HTTP_X_" + os.urandom(6).hex().upper(), os.urandom(64).hex()) for _ in range(50)]); n[0] += 1
            except Exception: errs[0] += 1; time.sleep(0.05)
    ts = [threading.Thread(target=w) for _ in range(16)]
    [t.start() for t in ts]; [t.join() for t in ts]
    print(n[0], errs[0])
PY
rss() { awk '/VmRSS/{print int($2/1024)}' "/proc/$pid/status" 2>/dev/null; }
fail=0
( cd "$TMP" && exec "$FCGI" --port "$PORT" --workers 8 --script "$TMP/app.lk" > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" leak >/dev/null 2>&1 && break; sleep 0.1; done
got="$(python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" leak 2>&1)"
[ "$got" = "[][alice-token-123] [][]" ] && echo "  OK   a header reaches request::header, never env(), and not the next request" \
    || { echo "  FAIL leak check: '$got' (want '[][alice-token-123] [][]')"; fail=1; }
r0=$(rss)
read -r n errs <<< "$(python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" race 8 2>&1)"
r1=$(rss)
if kill -0 "$pid" 2>/dev/null && [ "${n:-0}" -gt 500 ]; then echo "  OK   $n requests with 50 new header names each, 16 threads: the server is up"
else echo "  FAIL the server died under new header names (requests: ${n:-?}, errors: ${errs:-?})"; fail=1; fi
[ -n "$r1" ] && [ $((r1 - r0)) -lt 6 ] && echo "  OK   memory grew $((r1 - r0)) MB (environment strings are not kept)" \
    || { echo "  FAIL memory grew $(( ${r1:-0} - ${r0:-0} )) MB"; fail=1; }
st="$(python3 "$TMP/c.py" "$PORT" "$TMP/app.lk" store 2>&1)"
case "$st" in *"cannot be under web root"*) echo "  OK   file::store still refuses an upload dir under the request's DOCUMENT_ROOT";;
  *) echo "  FAIL file::store under DOCUMENT_ROOT: '$st'"; fail=1;; esac
[ $fail = 0 ] && echo "PASS: FastCGI request parameters stay out of the process environment" \
             || { echo "FAIL: FastCGI request parameters and the environment"; tail -3 "$TMP/log.txt"; exit 1; }
