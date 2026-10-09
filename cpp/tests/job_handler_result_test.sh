#!/usr/bin/env bash
# LOOK 2: a job handler says what happened, and a retry waits.
#
#   return true    the job is done
#   return false   the job failed and is tried again LATER, up to its limit (3)
#   an error       the same as false; the error is logged
#   anything else  — most often a forgotten `return` — is a mistake of the handler: it is
#                  logged and the job is marked failed WITHOUT being run again
# In LOOK 1 a handler that returned nothing counted as failed and was run again at once: three
# runs in the same millisecond, so a handler that sent a mail and forgot `return true` sent it
# three times. And a retry after a real failure came immediately, which helps nothing when the
# cause is a mail server that did not answer for a moment.
# The wait doubles with every try: base, 2 x base, 4 x base (base: LOOK_JOBS_RETRY_SECONDS,
# 30 by default; 1 here).
# Usage: job_handler_result_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7757}"
cat > "$TMP/app.lk" <<LK
use json
use cache
use jobs
\$c = db::connect("sqlite://$TMP/jobs.db")
function ran(\$q) { cache::set("runs_" . \$q, (cache::get("runs_" . \$q) ?? 0) + 1, 600) }
jobs::worker("good",    function(\$job) { ran("good"); return true })
jobs::worker("noreturn", function(\$job) { ran("noreturn") })
jobs::worker("text",    function(\$job) { ran("text"); return "sent" })
jobs::worker("bad",     function(\$job) { ran("bad"); return false })
jobs::worker("boom",    function(\$job) { ran("boom"); throw "smtp down" })
route("GET", "/push", function() { foreach (["good", "noreturn", "text", "bad", "boom"] as \$q) { jobs::push(\$q, "p") }; return response::text("pushed") })
route("GET", "/drain", function() { jobs::run(0); return response::text("drained") })
route("GET", "/r", function() {
    \$o = []
    foreach (["good", "noreturn", "text", "bad", "boom"] as \$q) {
        \$s = jobs::stats(\$q)
        \$o[\$q] = (cache::get("runs_" . \$q) ?? 0) . "/" . \$s["done"] . "/" . \$s["pending"] . "/" . \$s["failed"]
    }
    return response::json(\$o)
})
LK
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    rm -f "$TMP"/jobs.db*
    ( cd "$TMP" && exec env $mode LOOK_JOBS_RETRY_SECONDS=1 "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    curl -s -m 5 "localhost:$PORT/push" >/dev/null
    curl -s -m 20 "localhost:$PORT/drain" >/dev/null; curl -s -m 20 "localhost:$PORT/drain" >/dev/null
    first="$(curl -s -m 5 "localhost:$PORT/r")"                 # two passes in a row: one run each
    for i in 1 2 3 4 5 6 7 8 9 10; do sleep 0.6; curl -s -m 20 "localhost:$PORT/drain" >/dev/null; done
    last="$(curl -s -m 5 "localhost:$PORT/r")"                  # after the waits (1 s, then 2 s)
    said1="$(grep -c "handler for queue 'noreturn' must return true (done) or false.*it returned nothing (null)" "$TMP/log.txt")"
    said2="$(grep -c "handler for queue 'text' must return true (done) or false.*it returned a string" "$TMP/log.txt")"
    boom="$(grep -c 'smtp down' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    # runs/done/pending/failed
    W1='{"good":"1/1/0/0","noreturn":"1/0/0/1","text":"1/0/0/1","bad":"1/0/1/0","boom":"1/0/1/0"}'
    W2='{"good":"1/1/0/0","noreturn":"1/0/0/1","text":"1/0/0/1","bad":"3/0/0/1","boom":"3/0/0/1"}'
    if [ "$first" = "$W1" ]; then echo "  OK   [$mode] two passes in a row: every handler ran once; a retry waits; a handler without a result is not run again"
    else echo "  FAIL [$mode] after two passes: $first"; echo "       want:             $W1"; fail=1; fi
    if [ "$last" = "$W2" ] && [ "$said1" = "1" ] && [ "$said2" = "1" ] && [ "$boom" = "3" ]; then echo "  OK   [$mode] later: false and an error were tried 3 times and failed; the two bad results were logged once each"
    else echo "  FAIL [$mode] at the end: $last (logged: no-return=$said1 text=$said2 errors=$boom)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: a job handler returns true or false, and a retry waits" || { echo "FAIL: job handler result"; exit 1; }
