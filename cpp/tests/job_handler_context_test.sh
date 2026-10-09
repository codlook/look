#!/usr/bin/env bash
# A job handler started from inside a request does not share that request.
#
# A route that calls jobs::run(0) runs the queue's handlers before it answers. The handler ran
# with the request's own context: request::get(...) in the handler read the caller's
# parameters (its session likewise), and response::status(...) / response::header(...) in the
# handler changed the caller's response — measured on 1.0.12: the route answered 503 because
# its handler said so. The handler has a context of its own now.
# Usage: job_handler_context_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7745}"
cat > "$TMP/app.lk" <<LK
use json
use cache
use jobs
\$c = db::connect("sqlite://$TMP/jobs.db")
jobs::worker("ctx", function(\$job) {
    cache::set("handler_saw", request::get("x") ?? "nothing", 600)
    cache::set("handler_ran", (cache::get("handler_ran") ?? 0) + 1, 600)
    response::status(503)
    response::header("X-From-Handler", "1")
    return true
})
route("GET", "/go", function() {
    jobs::push("ctx", "p")
    jobs::run(0)
    return response::json(["x" => request::get("x"), "handler_saw" => cache::get("handler_saw") ?? "unset", "ran" => cache::get("handler_ran") ?? 0])
})
LK
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    rm -f "$TMP"/jobs.db*
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/nope" && break; sleep 0.1; done
    got="$(curl -s -m 10 -D "$TMP/h.txt" -w ' %{http_code}' "localhost:$PORT/go?x=SECRET")"
    hdr="$(grep -ci 'x-from-handler' "$TMP/h.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = '{"x":"SECRET","handler_saw":"nothing","ran":1} 200' ] && [ "$hdr" = "0" ]; then
        echo "  OK   [$mode] the handler ran once with its own context; the request kept its parameters, status and headers"
    else echo "  FAIL [$mode] response=[$got] handler-header-in-response=$hdr"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: a job handler does not share the request that started it" || { echo "FAIL: job handler context"; exit 1; }
