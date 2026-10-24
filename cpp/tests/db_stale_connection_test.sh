#!/usr/bin/env bash
# A pooled connection the database server closed is replaced before the query is sent.
#
# MySQL closes a connection idle longer than wait_timeout (8 h by default); a restart or KILL
# does the same, and PostgreSQL's pg_terminate_backend. The next query on that connection
# failed: a web application answered 500 to the first request of the morning, a script stopped.
# Now an idle connection that the server closed is found before anything is sent and replaced
# by a fresh one. A query that was already sent is never sent again (it could write twice), and
# inside a transaction there is no new connection: the transaction is gone, so it is an error.
# Needs live MySQL and PostgreSQL servers; the test closes their connections with a second
# LOOK process.
# Usage: db_stale_connection_test.sh <lk> <lk-fcgi> <mysql-dsn> <mysql-user> <pg-dsn> <pg-user>
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
MY_DSN="${3:?usage}"; MY_USER="${4:?usage}"; PG_DSN="${5:?usage}"; PG_USER="${6:?usage}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null' EXIT
cd "$TMP"
cat > killall.lk <<'LK'
use db
$k = db::connect(env("DSN"))
$n = 0
if (env("KIND") == "mysql") {
    foreach (db::query($k, "SELECT id FROM information_schema.processlist WHERE user = ? AND id <> CONNECTION_ID()", [env("KUSER")]) as $r) {
        try { db::exec($k, "KILL " . $r["id"], []); $n = $n + 1 } catch ($e) { }
    }
} else {
    foreach (db::query($k, "SELECT pg_terminate_backend(pid) AS ok FROM pg_stat_activity WHERE usename = ? AND pid <> pg_backend_pid() AND datname = current_database()", [env("KUSER")]) as $r) { $n = $n + 1 }
}
print("closed " . $n)
LK
cat > setup.lk <<'LK'
use db
$c = db::connect(env("DSN"))
db::exec($c, "DROP TABLE IF EXISTS look_stale", [])
db::exec($c, "CREATE TABLE look_stale (v INT)", [])
print("ready")
LK
# Plain queries: the connection is closed between them; the INSERT runs exactly once.
cat > plain.lk <<'LK'
use db
use file
$c = db::connect(env("DSN"))
print("first " . db::col($c, "SELECT 1", []))
file::put("ready", "1")
while (!file::exists("go")) { }
db::exec($c, "INSERT INTO look_stale (v) VALUES (1)", [])
print("rows " . db::col($c, "SELECT COUNT(*) FROM look_stale WHERE v = 1", []))
LK
# Inside a transaction: the connection is closed in the middle; no silent new connection.
cat > tx.lk <<'LK'
use db
use file
$c = db::connect(env("DSN"))
try {
    db::transaction($c, function() use ($c) {
        db::exec($c, "INSERT INTO look_stale (v) VALUES (2)", [])
        file::put("ready", "1")
        while (!file::exists("go")) { }
        db::exec($c, "INSERT INTO look_stale (v) VALUES (2)", [])
        return true
    })
    print("committed")
} catch ($e) {
    print("error " . $e)
}
print("rows " . db::col($c, "SELECT COUNT(*) FROM look_stale WHERE v = 2", []))
LK
cat > web.lk <<'LK'
use db
$c = db::connect(env("DSN"))
route("GET", "/q", fn() => response::text("ok " . db::col($c, "SELECT 1", [])))
route("GET", "/ok", fn() => response::text("alive"))
LK
fail=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }
run_with_close() {   # <script> : runs it, closes the server-side connections at "ready", then "go"
    rm -f ready go
    "$LK" "$1" > out.txt 2>&1 & local cp=$!
    for i in $(seq 1 100); do [ -f ready ] && break; sleep 0.1; done
    "$LK" killall.lk > /dev/null 2>&1; sleep 0.3; touch go
    for i in $(seq 1 100); do kill -0 "$cp" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$cp" 2>/dev/null; wait "$cp"; echo $?
}
for kind in mysql pg; do
    if [ $kind = mysql ]; then export DSN="$MY_DSN" KUSER="$MY_USER"; else export DSN="$PG_DSN" KUSER="$PG_USER"; fi
    export KIND=$kind
    "$LK" setup.lk > /dev/null 2>&1 || { echo "  FAIL $kind: cannot create the test table"; fail=1; continue; }
    echo "== $kind"
    rc=$(run_with_close plain.lk)
    check "$kind: the query after the server closed the connection works (exit $rc)" '[ "$rc" = 0 ] && grep -q "^first 1$" out.txt'
    check "$kind: the INSERT ran exactly once" 'grep -q "^rows 1$" out.txt'
    [ $fail = 0 ] || sed 's/^/       | /' out.txt | grep -vE "INFO|WARN" | head -4
    rc=$(run_with_close tx.lk)
    check "$kind: inside a transaction it is an error, not a silent new connection" 'grep -q "^error .*transaction" out.txt'
    check "$kind: nothing of that transaction was kept" 'grep -q "^rows 0$" out.txt'
    grep -qE "^error|^committed" out.txt || sed 's/^/       | /' out.txt | grep -vE "INFO|WARN" | head -4
    for mode in fcgi http; do
        port=$([ $mode = fcgi ] && echo 7791 || echo 7792)
        if [ $mode = fcgi ]; then "$FCGI" --port $port --workers 2 web.lk > srv.log 2>&1 & pid=$!
        else "$FCGI" --mode http --port $port --workers 2 web.lk > srv.log 2>&1 & pid=$!; fi
        sleep 1.5
        q() { if [ $mode = fcgi ]; then python3 "$HERE/fcgi_get.py" $port "$1" "$TMP/web.lk" 2>&1 | tail -1
              else curl -s -m 5 "localhost:$port$1"; fi; }
        a="$(q /q)"; "$LK" killall.lk > /dev/null 2>&1; sleep 0.3; b="$(q /q)"
        check "$kind, $mode: the first request after the server closed the connections answers ('$a' then '$b')" '[ "$a" = "ok 1" ] && [ "$b" = "ok 1" ]'
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    done
done
[ $fail = 0 ] && echo "PASS: a connection the server closed is replaced before the query" || { echo "FAIL: stale database connection"; exit 1; }
