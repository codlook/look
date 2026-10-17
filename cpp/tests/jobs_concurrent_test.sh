#!/usr/bin/env bash
# A job is handled exactly once, also when several consumers drain the queue at the same time.
#
# A queue can be drained from more than one place at once: a timer that calls jobs::run(0),
# a route that does the same, several workers. Taking the next job and marking it as taken
# must be one atomic step, or two consumers get the same job: two e-mails, two charges. That
# bug existed once (JobStore::next used a SELECT followed by an UPDATE) and was fixed with a
# single UPDATE ... WHERE status='pending' RETURNING; this test keeps it fixed.
# 300 jobs, 8 workers, 8 requests draining at once: 300 handled, 300 distinct.
# Usage: jobs_concurrent_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7725}"
cat > "$TMP/app.lk" <<LK
use json
use jobs
\$c = db::connect("sqlite://$TMP/seen.db")
db::exec(\$c, "CREATE TABLE IF NOT EXISTS seen (job_id INTEGER)")
function conn() { return db::connect("sqlite://$TMP/seen.db") }
jobs::worker("q", function(\$job) { db::exec(conn(), "INSERT INTO seen (job_id) VALUES (?)", [\$job["id"]]); return true })
route("GET", "/fill", function() { for (\$i = 0; \$i < 300; \$i = \$i + 1) { jobs::push("q", ["n" => \$i]) }; return response::json(1) })
route("GET", "/drain", function() { jobs::run(0); return response::json(1) })
route("GET", "/n", fn() => response::json(["handled" => db::query(conn(), "SELECT COUNT(*) AS c FROM seen")[0]["c"],
                                            "distinct" => db::query(conn(), "SELECT COUNT(DISTINCT job_id) AS c FROM seen")[0]["c"]]))
LK
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    rm -f "$TMP"/*.db*
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 8 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/n" && break; sleep 0.1; done
    curl -s -m 30 "localhost:$PORT/fill" >/dev/null
    pids=""; for i in 1 2 3 4 5 6 7 8; do curl -s -m 60 -o /dev/null "localhost:$PORT/drain" & pids="$pids $!"; done; wait $pids
    got="$(curl -s -m 5 "localhost:$PORT/n")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = '{"handled":300,"distinct":300}' ]; then echo "  OK   [$mode] 300 jobs, 8 consumers at once: each handled exactly once"
    else echo "  FAIL [$mode] $got (want 300 handled, 300 distinct)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: concurrent consumers never handle a job twice" || { echo "FAIL: a job was handled more than once or lost"; exit 1; }
