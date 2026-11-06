#!/usr/bin/env bash
# A hot reload while a request is still running does not free that request's program.
#
# The request let go of the app lock before its route ran and went on with raw pointers into the
# loaded program; a reload in the meantime (the file changed while the request waited on a query
# or an http::get) freed the old bytecode, and when the request resumed the process died with
# SIGSEGV (ASan: heap-use-after-free in VM::run) — with fibers and with the worker pool. Now every
# request holds the generation it started with (program, routes, setup values, dispatch tables);
# the old generation is freed when its last request ends. New requests get the new program.
# Usage: hot_reload_inflight_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; kill $pid $up 2>/dev/null' EXIT
PORT="${PORT:-7753}"; UP=$((PORT + 1)); pid=""; up=""
cat > "$TMP/slow.py" <<'PY'
import http.server, socketserver, time, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(3); b = b"slow"
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
write_v1() { cat > "$TMP/app.lk" <<LK
use http
function helper(\$x) { return "h" . \$x }
route("GET", "/slow", function() {
    \$r = http::get("http://127.0.0.1:$UP/s")
    \$s = ""
    foreach ([1, 2, 3] as \$v) { \$s = \$s . helper(\$v) }
    return response::text("slow-done " . \$s . " " . \$r["body"])
})
route("GET", "/fast", fn() => response::text("fast1"))
LK
}
write_v2() {
    { echo 'use http'
      for i in $(seq 1 300); do echo "function f$i(\$x) { return \"zz$i\" . \$x }"; done
      echo 'route("GET", "/fast", fn() => response::text("fast2"))'; } > "$TMP/app.lk.new"
    touch -d "+5 seconds" "$TMP/app.lk.new"; mv "$TMP/app.lk.new" "$TMP/app.lk"
}
python3 "$TMP/slow.py" "$UP" & up=$!
fail=0
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
    write_v1
    ( cd "$TMP" && exec env $mode LOOK_ALLOW_SSRF=1 LOOK_HOT_RELOAD_MS=0 "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 100); do [ "$(curl -s -m 1 "localhost:$PORT/fast")" = fast1 ] && break; sleep 0.1; done
    curl -s -m 30 "localhost:$PORT/slow" > "$TMP/slow.out" & c=$!
    sleep 0.8
    write_v2
    new="$(for i in 1 2 3; do curl -s -m 10 "localhost:$PORT/fast"; echo -n ' '; done)"
    wait $c
    m="${mode:-fibers}"
    [ "$(cat "$TMP/slow.out")" = "slow-done h1h2h3 slow" ] && echo "  OK   [$m] the running request finished on its own program" \
        || { echo "  FAIL [$m] running request: '$(cat "$TMP/slow.out")'"; fail=1; }
    case "$new" in *fast2*) echo "  OK   [$m] new requests got the reloaded program";; *) echo "  FAIL [$m] after reload: '$new'"; fail=1;; esac
    kill -0 "$pid" 2>/dev/null && echo "  OK   [$m] the server is still running" || { echo "  FAIL [$m] the server died"; tail -3 "$TMP/log.txt"; fail=1; }
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
done
[ $fail = 0 ] && echo "PASS: a hot reload never frees a running request's program" \
             || { echo "FAIL: hot reload during a running request"; exit 1; }
