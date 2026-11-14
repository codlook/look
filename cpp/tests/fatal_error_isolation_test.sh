#!/usr/bin/env bash
# A request that dies mid-way (out of memory, throw, stack overflow, exit, an error in before_route)
# leaves nothing behind for the next request on the same worker, and is answered as an error.
#
# The PHP2Shell class (CVE-2026-109618 / -109619): an out-of-memory fatal error in the middle of a
# request left per-worker state behind, and the next request on that worker reused it (use after
# free, memory disclosure in a session cookie). This test runs one worker under a 2 GB virtual
# memory limit, makes a request set a header, a cookie, a status, a session and partial output and
# then fail in each of those ways, and checks that a plain request right after it sees none of it.
# With glibc heap checking (MALLOC_PERTURB_ fills freed memory, MALLOC_CHECK_=3 aborts on heap
# corruption) stale memory would show up in the answers or stop the server.
# Also: a request that failed after response::status(201) was answered "201 Created" (only a 200 was
# turned into 500); a failed request is now 500 unless the app chose a 4xx/5xx itself.
# Both with the memory limit off (a real std::bad_alloc) and on (the request is stopped first).
# Usage: fatal_error_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7759}"
cat > "$TMP/app.lk" <<'LK'
use string
function deep($n) { return deep($n + 1) }
before_route(function() {
    if (request::path() == "/boom-before") { response::header("X-Leak", "b"); throw "before failed" }
})
route("GET", "/oom", function() {
    response::header("X-Leak", "secret-header"); cookie::set("leak", "secret-cookie"); response::status(201)
    session::start(); session::set("u", "failed-request")
    print("partial-output-")
    $s = string::repeat("A", 1000000)
    for ($i = 0; $i < 40; $i++) { $s = $s . $s }
    return response::text("not reached " . strlen($s))
})
route("GET", "/throw", function() { response::header("X-Leak", "t"); cookie::set("leak", "t"); response::status(302); print("partial-"); throw "boom" })
route("GET", "/deep", function() { response::header("X-Leak", "d"); print("partial-"); return deep(0) })
route("GET", "/exit", function() { response::header("X-Leak", "e"); print("exit-out"); exit() })
route("GET", "/boom-before", fn() => response::text("unreached"))
route("GET", "/clean", fn() => response::text("clean:" . (session::get("u") ?? "none")))
LK
# İki aşama, aynı yedi istek:
#   off  — LOOK_MEMORY_LIMIT=off: gerçek bellek tükenmesi (std::bad_alloc) yolu
#   on   — bellek bütçesi açık (include/look/mem_budget.h): istek bütçede durur, sunucu ayakta kalır
fail=0
phase() { # <label> <LOOK_MEMORY_LIMIT> <log pattern that proves the case ran> <what it proves>
    : > "$TMP/log.txt"; rm -rf "$TMP/s"
    ( cd "$TMP" && ulimit -v 2097152 && exec env LOOK_MEMORY_LIMIT="$2" MALLOC_PERTURB_=165 MALLOC_CHECK_=3 LOOK_SECRET=fatal-0123456789abcdef0123456789abcdef \
        LOOK_SESSION_DIR="$TMP/s" "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/clean" && break; sleep 0.1; done
    local pf=0
    for r in oom throw deep exit boom-before oom oom; do
        code="$(curl -s -m 30 -o /dev/null -w '%{http_code}' "localhost:$PORT/$r")"
        want=500; [ $r = exit ] && want=200
        [ "$code" = "$want" ] || { echo "  FAIL [$1] /$r answered $code (want $want)"; pf=1; }
        h="$(curl -s -m 10 -D - "localhost:$PORT/clean")"; body="$(echo "$h" | tail -1)"; leak=""
        echo "$h" | grep -qi "^x-leak" && leak="$leak header"
        echo "$h" | grep -qi "^set-cookie: leak" && leak="$leak cookie"
        echo "$h" | head -1 | grep -qv " 200 " && leak="$leak status"
        [ "$body" = "clean:none" ] || leak="$leak body($body)"
        [ -z "$leak" ] || { echo "  FAIL [$1] after /$r the next request saw:$leak"; pf=1; }
    done
    [ $pf = 0 ] && echo "  OK   [$1] 7 failed requests (3 out of memory): every next request clean, every failure an error status"
    grep -q "$3" "$TMP/log.txt" && echo "  OK   [$1] $4" \
        || { echo "  FAIL [$1] the log does not show the case ran ($3)"; tail -3 "$TMP/log.txt"; pf=1; }
    kill -0 "$pid" 2>/dev/null && echo "  OK   [$1] the server is still running" || { echo "  FAIL [$1] the server died"; tail -3 "$TMP/log.txt"; pf=1; }
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ $pf = 0 ] || fail=1
}
phase off off "std::bad_alloc" "the out-of-memory case really ran out of memory (std::bad_alloc)"
phase on 512MiB "of memory and was stopped" "the memory limit stopped the request before memory ran out"
[ $fail = 0 ] && echo "PASS: a request that dies leaves nothing for the next one" || { echo "FAIL: state after a failed request"; exit 1; }
