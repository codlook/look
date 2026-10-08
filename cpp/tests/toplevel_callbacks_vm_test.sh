#!/usr/bin/env bash
# LOOK 2: callbacks registered at the top level run on the VM — a timer armed there and a
# job handler registered with jobs::worker.
#
# The top level of a web application runs first on the tree-walk engine. A timer or a job
# handler registered there was a tree-walk function and kept running on that engine while the
# rest of the application ran on the VM: two engines side by side, the slow one doing the
# background work. When the VM's setup has succeeded, those registrations are now handed over
# to the VM's own closures (look/setup_handoff.h). The timer keeps its id.
# How this test sees the engine: each callback runs a loop the tree-walk engine needs seconds
# for and the VM a fraction of one, and the test measures from outside how long it took.
#   * VM mode: both callbacks finish fast, the server says how many it handed over
#   * LOOK_BYTECODE=0: the same application works, on the tree-walk engine (slow) — which also
#     shows the loop is long enough to tell the engines apart
#   * a job's result is respected: true = done; false or an error = tried again up to its
#     limit (3), then failed — the same count of handler runs on both engines
#   * a timer id taken at the top level still cancels that timer from a route
#   * a global written by a callback does not reach a later request
#   * a job handler run from inside a request (a route calls jobs::run) gets its own request
#     context, and the request that ran it keeps its own status, headers and parameters
# Usage: toplevel_callbacks_vm_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7739}"
cat > "$TMP/app.lk" <<LK
use json
use cache
use jobs
\$c = db::connect("sqlite://$TMP/jobs.db")
\$G = ["g0"]
function spin() { \$s = 0; for (\$i = 0; \$i < 1500000; \$i++) { \$s = \$s + \$i % 7 }; return \$s }
jobs::worker("mail", function(\$job) {
    spin(); \$G[0] = "job"
    cache::set("job_runs", (cache::get("job_runs") ?? 0) + 1, 600)
    if (\$job["payload"] == "bad") { return false }
    if (\$job["payload"] == "boom") { throw "boom in job" }
    return true
})
timer::after(300, function() { spin(); \$G[0] = "timer"; cache::set("timer_done", 1, 600) })
\$tick = timer::every(100, function() { cache::set("ticks", (cache::get("ticks") ?? 0) + 1, 600) })
route("GET", "/push", function() { jobs::push("mail", request::get("to")); return response::text("pushed") })
route("GET", "/drain", function() { jobs::run(0); return response::text("drained") })
route("GET", "/stop", function() { timer::cancel(\$tick); return response::text("stopped") })
jobs::worker("ctx", function(\$job) {
    cache::set("handler_saw", request::get("x") ?? "nothing", 600)
    response::status(503); response::header("X-From-Handler", "1")
    return true
})
route("GET", "/nest", function() {
    jobs::push("ctx", "p")
    jobs::run(0)
    return response::json(["x" => request::get("x"), "handler_saw" => cache::get("handler_saw") ?? "unset"])
})
route("GET", "/r", fn() => response::json(["global" => \$G[0], "timer_done" => cache::get("timer_done") ?? 0,
      "job_runs" => cache::get("job_runs") ?? 0, "ticks" => cache::get("ticks") ?? 0, "stats" => jobs::stats("mail")]))
LK
num() { echo "$1" | grep -o "\"$2\":[0-9-]*" | head -1 | cut -d: -f2; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    rm -f "$TMP"/jobs.db*
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 60); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.05; done
    t0=$(now_ms); tm=-1
    for i in $(seq 1 200); do [ "$(num "$(curl -s -m 3 "localhost:$PORT/r")" timer_done)" = "1" ] && { tm=$(( $(now_ms) - t0 )); break; }; sleep 0.05; done
    curl -s -m 3 "localhost:$PORT/push?to=a" >/dev/null
    j0=$(now_ms); curl -s -m 60 "localhost:$PORT/drain" >/dev/null; jm=$(( $(now_ms) - j0 ))
    for to in bad boom; do curl -s -m 3 "localhost:$PORT/push?to=$to" >/dev/null; done
    curl -s -m 60 "localhost:$PORT/drain" >/dev/null
    nest="$(curl -s -m 10 -D "$TMP/h.txt" -w ' %{http_code}' "localhost:$PORT/nest?x=42")"; nh="$(grep -ci 'x-from-handler' "$TMP/h.txt")"
    after="$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/r")"
    t1="$(num "$(curl -s -m 3 "localhost:$PORT/r")" ticks)"; curl -s -m 3 "localhost:$PORT/stop" >/dev/null; sleep 0.3
    t2="$(num "$(curl -s -m 3 "localhost:$PORT/r")" ticks)"; sleep 0.5; r="$(curl -s -m 3 "localhost:$PORT/r")"; t3="$(num "$r" ticks)"
    said="$(grep -c 'Top-level callbacks on the VM: 2 timer, 2 job handler' "$TMP/log.txt")"
    boom="$(grep -c 'boom in job' "$TMP/log.txt")"
    vmerr="$(grep -c 'VM BUG\|runs on the interpreter' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    base="$(echo "$r" | grep -o '"global":"[a-z0-9]*"') runs=$(num "$r" job_runs) done=$(num "$r" done) failed=$(num "$r" failed)"
    if [ "$base" = '"global":"g0" runs=7 done=1 failed=2' ] && [ "$boom" = "3" ] && [ "${t1:-0}" -ge 2 ] && [ "$t2" = "$t3" ]; then
        echo "  OK   [$mode] 3 jobs: 1 done, 2 failed after 3 tries each (7 handler runs, each error logged); the top-level timer id cancels it; globals untouched"
    else echo "  FAIL [$mode] $base boom-logged=$boom ticks=$t1/$t2/$t3 r=$r"; fail=1; fi
    if [ "$nest" = '{"x":"42","handler_saw":"nothing"} 200' ] && [ "$nh" = "0" ] && [ "$after" = "200" ]; then
        echo "  OK   [$mode] a handler run from inside a request has its own context; the request keeps its status, headers and parameters"
    else echo "  FAIL [$mode] nested handler: response=[$nest] handler-header-leaked=$nh next-request=$after"; fail=1; fi
    if [ "$mode" = "LOOK_VM_STRICT=1" ]; then
        if [ "$said" = "1" ] && [ "$vmerr" = "0" ] && [ "$tm" -ge 0 ] && [ "$tm" -lt 1200 ] && [ "$jm" -lt 900 ]; then
            echo "  OK   [$mode] both callbacks ran on the VM (timer done after ${tm} ms, one job drained in ${jm} ms)"
        else echo "  FAIL [$mode] handed-over line=$said timer=${tm} ms (want < 1200) job=${jm} ms (want < 900) log-problems=$vmerr"; fail=1; fi
    else
        if [ "$said" = "0" ] && [ "$tm" -ge 1500 ] && [ "$jm" -ge 1200 ]; then echo "  OK   [$mode] on the tree-walk engine the same callbacks take ${tm} / ${jm} ms — the loop tells the engines apart"
        else echo "  FAIL [$mode] expected slow tree-walk callbacks: timer=${tm} ms job=${jm} ms handed-over line=$said"; fail=1; fi
    fi
done
[ $fail = 0 ] && echo "PASS: top-level timers and job handlers run on the VM" || { echo "FAIL: top-level callbacks"; exit 1; }
