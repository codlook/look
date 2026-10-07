#!/usr/bin/env bash
# LOOK 2: an application that uses the job queue runs on the VM.
#
# The usual shape is a handler registered at the top level and a timer that drains the queue:
#     jobs::worker("mail", function($job) { ... })
#     timer::every(100, fn() => jobs::run(0))
# jobs::worker was not a VM builtin and timer:: could not be called in the VM setup pass, so
# either line sent the WHOLE application to the tree-walk engine, and LOOK_VM_STRICT=1 refused
# to start. Now the setup pass replays both (the handler is registered once, the timer armed
# once), jobs::run is a VM builtin, and routes stay on the VM.
#   * the application starts on the VM, also with LOOK_VM_STRICT=1
#   * a pushed job is handled exactly once
#   * jobs::run(0) called from a route drains the queue as well
#   * nothing is placed on the tree-walk engine, nothing falls back
# Usage: jobs_vm_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7724}"
cat > "$TMP/app.lk" <<LK
use json
use jobs
use cache
\$c = db::connect("sqlite://$TMP/jobs.db")
function handled() { return cache::get("handled") ?? 0 }
jobs::worker("mail", function(\$job) { cache::set("handled", handled() + 1, 600); return true })
jobs::worker("manual", function(\$job) { cache::set("manual", (cache::get("manual") ?? 0) + 1, 600); return true })
\$tick = timer::every(60, fn() => jobs::run(0))
route("GET", "/push", function() { jobs::push("mail", ["to" => request::get("to")]); return response::json(1) })
route("GET", "/push-manual", function() { jobs::push("manual", ["n" => 1]); return response::json(1) })
route("GET", "/stop", function() { timer::cancel(\$tick); return response::json(1) })
route("GET", "/drain", function() { jobs::run(0); return response::json(1) })
route("GET", "/n", fn() => response::json(["handled" => handled(), "manual" => cache::get("manual") ?? 0]))
LK
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1"; do
    rm -f "$TMP"/jobs.db*
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/n" && break; sleep 0.1; done
    for t in a b c; do curl -s -m 3 "localhost:$PORT/push?to=$t" >/dev/null; done; sleep 0.6
    n1="$(curl -s -m 3 "localhost:$PORT/n")"; sleep 0.4; n2="$(curl -s -m 3 "localhost:$PORT/n")"
    curl -s -m 3 "localhost:$PORT/stop" >/dev/null; sleep 0.2
    curl -s -m 3 "localhost:$PORT/push-manual" >/dev/null; sleep 0.4
    before="$(curl -s -m 3 "localhost:$PORT/n")"
    curl -s -m 5 "localhost:$PORT/drain" >/dev/null
    after="$(curl -s -m 3 "localhost:$PORT/n")"
    vmroutes="$(grep -o 'VM routes: [0-9]* registered' "$TMP/log.txt")"
    bad="$(grep -c 'runs on the interpreter\|VM BUG\|interpreter fallback\|setup replay\|VM setup' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$vmroutes" = "VM routes: 5 registered" ] && [ "$bad" = "0" ]; then echo "  OK   [$mode] the application runs on the VM: 5 routes, nothing on the tree-walk engine"
    else echo "  FAIL [$mode] '$vmroutes', interpreter/fallback/setup log lines: $bad $(grep -m1 'replay\|VM setup\|Error' "$TMP/log.txt" | cut -c1-160)"; fail=1; fi
    if [ "$n1" = '{"handled":3,"manual":0}' ] && [ "$n2" = "$n1" ]; then echo "  OK   [$mode] three pushed jobs were handled once each by the timer-driven worker"
    else echo "  FAIL [$mode] after 3 pushes: $n1 then $n2"; fail=1; fi
    if [ "$before" = '{"handled":3,"manual":0}' ] && [ "$after" = '{"handled":3,"manual":1}' ]; then echo "  OK   [$mode] timer::cancel with the id from setup stopped the worker; jobs::run(0) in a route drained the queue"
    else echo "  FAIL [$mode] before drain: $before after: $after"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: a job-queue application runs on the VM" || { echo "FAIL: jobs on the VM"; exit 1; }
