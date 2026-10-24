#!/usr/bin/env bash
# A database connection closed by the server does not kill the LOOK process.
#
# The DB clients wrote with plain send(), and only `lk-fcgi --mode http` ignored SIGPIPE. When
# MySQL closed an idle pooled connection (wait_timeout, a restart, KILL), the next write raised
# SIGPIPE and the process died (exit 141): lk-fcgi in FastCGI mode — the Plesk default — took
# every site on the binary down until systemd restarted it; a long-running `lk` worker died.
# Now every LOOK program ignores SIGPIPE (lk still ends quietly when its own output pipe closes,
# as before) and the DB clients write with MSG_NOSIGNAL.
# Needs a live MySQL server; the test kills its connections with a second LOOK process.
# Usage: db_dropped_connection_test.sh <lk> <lk-fcgi> <mysql-dsn> <mysql-user>
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
DSN="${3:?usage}"; USER_="${4:?usage}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null' EXIT
export DSN KUSER="$USER_"
cd "$TMP"
cat > killall.lk <<'LK'
use db
$k = db::connect(env("DSN"))
$n = 0
foreach (db::query($k, "SELECT id FROM information_schema.processlist WHERE user = ? AND id <> CONNECTION_ID()", [env("KUSER")]) as $r) {
    try { db::exec($k, "KILL " . $r["id"], []); $n = $n + 1 } catch ($e) { }
}
print("killed " . $n)
LK
cat > cli.lk <<'LK'
use db
use file
$c = db::connect(env("DSN"))
print("first " . db::col($c, "SELECT 1", []))
file::put("ready", "1")
while (!file::exists("go")) { }
print("second " . db::col($c, "SELECT 2", []))
print("end")
LK
cat > web.lk <<'LK'
use db
$c = db::connect(env("DSN"))
route("GET", "/q", fn() => response::text("ok " . db::col($c, "SELECT 1", [])))
route("GET", "/ok", fn() => response::text("alive"))
LK
killall() { "$LK" killall.lk 2>/dev/null | tail -1; }
fail=0

echo "== lk: a pooled connection is killed between two queries"
rm -f ready go
"$LK" cli.lk > cli.out 2>&1 & cp=$!
for i in $(seq 1 100); do [ -f ready ] && break; sleep 0.1; done
k="$(killall)"; touch go
for i in $(seq 1 100); do kill -0 "$cp" 2>/dev/null || break; sleep 0.1; done
kill -9 "$cp" 2>/dev/null; wait "$cp"; rc=$?
# Not killed by a signal: the query on the dead connection either works or fails with a LOOK
# error the script can catch. (Retrying it on a fresh connection is a separate matter.)
if [ "$rc" != 141 ] && { grep -q "^second 2$" cli.out || grep -q "connection lost" cli.out; }; then
    echo "  OK   lk is not killed; the dead connection gives: $(grep -E '^second|connection lost' cli.out | head -1 | sed 's/^ *//') ($k)"
else echo "  FAIL lk exit $rc ($k): $(grep -vE 'INFO|WARN' cli.out | tr '\n' ' ' | head -c 200)"; fail=1; fi

for mode in fcgi http; do
    echo "== lk-fcgi --mode $mode: the server's connections are killed between requests"
    [ $mode = fcgi ] && port=7781 || port=7782
    if [ $mode = fcgi ]; then "$FCGI" --port $port --workers 2 web.lk > srv.log 2>&1 & pid=$!
    else "$FCGI" --mode http --port $port --workers 2 web.lk > srv.log 2>&1 & pid=$!; fi
    sleep 1.5
    q() { if [ $mode = fcgi ]; then python3 "$HERE/fcgi_get.py" $port "$1" "$TMP/web.lk" 2>&1 | tail -1
          else curl -s -m 5 "localhost:$port$1"; fi; }
    a="$(q /q)"; k="$(killall)"; b="$(q /q)"; c="$(q /q)"; sleep 0.3
    if kill -0 "$pid" 2>/dev/null; then
        if [ "$a" = "ok 1" ] && [ "$c" = "ok 1" ]; then echo "  OK   the server stays up and answers ($k; after: '$b', '$c')"
        else echo "  FAIL the server is up but answers '$a' / '$b' / '$c' ($k)"; fail=1; fi
    else wait "$pid"; echo "  FAIL the server died (exit $?) after its connections were killed ($k)"; fail=1; fi
    kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
done

echo "== lk still ends when its output pipe closes"
printf 'while (true) { print("x") }\n' > loop.lk
timeout 10 bash -c "'$LK' loop.lk | head -1 > /dev/null"; rc=$?
[ "$rc" != 124 ] && echo "  OK   'lk loop.lk | head -1' ends" || { echo "  FAIL 'lk loop.lk | head -1' keeps running"; fail=1; }

[ $fail = 0 ] && echo "PASS: a dropped database connection does not kill LOOK" || { echo "FAIL: dropped database connection"; exit 1; }
