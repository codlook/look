#!/usr/bin/env bash
# A session has a size limit: going over it is an error, never a silent cut.
#
# A session had no limit (only Redis's 64 MB single-reply limit): a visitor could grow their own
# session to megabytes, which was then read on every request, and a few visitors could fill
# Redis's memory. LOOK 2: at most LOOK_SESSION_MAX_BYTES (default 1 MB) per session, the same for
# the file and the Redis driver. session::set over the limit throws an error naming the limit;
# nothing is written and the session keeps what it had. The application can catch the error.
# Usage: session_size_limit_test.sh <lk-fcgi> [redis-url]   (needs curl)
set -u
# LOOK 2: çerez kullanan uygulama LOOK_SECRET olmadan başlamaz (CSRF imzası) — testin kendi anahtarı.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
RURL="${2:-}"
P=7498
T=$(mktemp -d); trap '{ kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""
cat > "$T/app.lk" <<'LK'
use string;
route("GET", "/set", function() { session::start(); session::set("v", string::repeat("a", int(request::get("n")))); return response::text("ok") })
route("GET", "/len", function() { session::start(); return response::text("len=" . string::len(session::get("v") ?? "")) })
route("GET", "/caught", function() {
    session::start()
    try { session::set("v", string::repeat("b", int(request::get("n")))) } catch ($e) { return response::text("caught: " . $e) }
    return response::text("no error")
})
LK
fail=0
start() {   # $1 = ek ortam
    { kill $pid; wait $pid; } 2>/dev/null
    rm -f "$T/cj"
    if [ -n "$RURL" ]; then drv="LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL=$RURL"; else drv=""; fi
    ( cd "$T" && exec env $drv $1 LOOK_SESSION_SECURE=0 TEMP="$T" "$LKF" --mode http --port $P --workers 2 app.lk >"$T/srv.log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P/health" >/dev/null && break; sleep 0.2; done
}
q() { curl -s -b "$T/cj" -c "$T/cj" --max-time 10 -o "$T/body" -w '%{http_code}' "http://127.0.0.1:$P$1"; }
chk() { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }
label="${RURL:+redis}"; label="${label:-file}"

start ""
chk "[$label] default limit: a 900 KB session is stored" "$(q '/set?n=900000')" 200
chk "[$label] default limit: a 1.1 MB session is an error" "$(q '/set?n=1100000')" 500
grep -q "LOOK_SESSION_MAX_BYTES" "$T/srv.log" && echo "  OK   [$label] the error names LOOK_SESSION_MAX_BYTES (logged)" \
    || { echo "  FAIL [$label] the logged error does not name LOOK_SESSION_MAX_BYTES"; tail -3 "$T/srv.log"; fail=1; }
q /len >/dev/null; chk "[$label] the session keeps what it had (nothing cut, nothing written)" "$(cat "$T/body")" "len=900000"
q '/caught?n=1100000' >/dev/null
case "$(cat "$T/body")" in "caught: session::set() — the session would be"*) echo "  OK   [$label] try/catch sees the error";;
    *) echo "  FAIL [$label] try/catch: '$(head -c 120 "$T/body")'"; fail=1;; esac

start "LOOK_SESSION_MAX_BYTES=5000"
chk "[$label] LOOK_SESSION_MAX_BYTES=5000: 4000 bytes stored" "$(q '/set?n=4000')" 200
chk "[$label] LOOK_SESSION_MAX_BYTES=5000: 6000 bytes refused" "$(q '/set?n=6000')" 500

[ $fail = 0 ] && echo "PASS: session size limit ($label driver)" || echo "FAIL: session size limit ($label driver)"
exit $fail
