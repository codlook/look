#!/usr/bin/env bash
# MySQL and PostgreSQL: after a reply the client could not read to its end, the next queries get
# their own results.
#
# The clients read replies through a buffer (one recv for many packets). When a reply was refused
# half-way (a malformed row, a column count over the limit) its unread rest stayed in that buffer.
# Before each query the client checks the socket for leftovers — but bytes already in the buffer
# are not on the socket: the next query read the rest of the old reply as its own (no rows), the
# query after it got the previous query's row, and so on, with no error. LOOK 1 read without a
# buffer and its socket check caught the leftovers. Now a reply that cannot be read to its end
# closes the connection, and leftovers in the buffer (or in TLS) count as a stale connection too.
# Fake servers send the bad reply for the first query only.
# Usage: db_malformed_desync_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; S1=""; S2=""
trap '[ -n "$S1" ] && kill $S1 2>/dev/null; [ -n "$S2" ] && kill $S2 2>/dev/null; rm -rf "$T"' EXIT
command -v python3 >/dev/null || { echo "FAIL: python3 is needed"; exit 1; }
PP=$((21900 + RANDOM % 40)); MP=$((21950 + RANDOM % 40))
python3 "$HERE/fake_pg_server.py" desync $PP > "$T/pg.log" 2>&1 & S1=$!
python3 "$HERE/fake_mysql_server.py" desync $MP > "$T/my.log" 2>&1 & S2=$!
for i in $(seq 1 50); do grep -q hazir "$T/pg.log" 2>/dev/null && grep -q hazir "$T/my.log" 2>/dev/null && break; sleep 0.1; done
fail=0
for db in "PostgreSQL|postgres://u:p@127.0.0.1:$PP/d" "MySQL|mysql://u:p@127.0.0.1:$MP/d"; do
    name=${db%%|*}; dsn=${db#*|}
    cat > "$T/q.lk" <<LK
use db
\$c = db::connect("$dsn")
\$a = "no-error"
try { db::query(\$c, "SELECT 'FIRST' AS col", []) } catch (\$e) { \$a = "error" }
\$b = db::query(\$c, "SELECT 'SECOND' AS col", [])
\$d = db::query(\$c, "SELECT 'THIRD' AS col", [])
print(\$a . "|" . count(\$b) . ":" . (count(\$b) > 0 ? \$b[0]["col"] : "-") . "|" . count(\$d) . ":" . (count(\$d) > 0 ? \$d[0]["col"] : "-"))
LK
    out=$(cd "$T" && timeout 30 "$LK" q.lk 2>&1 | grep -v '^\[20' | tail -1)
    if [ "$out" = "error|1:second|1:third" ]; then echo "  OK   $name: a refused reply, then the next two queries get their own rows"
    else echo "  FAIL $name: got '$out', want 'error|1:second|1:third' (a result one query behind = the old reply's rest was read)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: no result shifts after a refused reply" || echo "FAIL: reply desync"
exit $fail
