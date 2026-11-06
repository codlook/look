#!/usr/bin/env bash
# MySQL statement cache behind a connection multiplexer (ProxySQL and similar): when the server
# answers EXECUTE with 1243 (unknown statement — the multiplexer moved the session to another
# backend), the statement is prepared again and the query runs once; nothing else is retried.
# Error 1243 means the query did not run, so one new attempt is safe. The fake server runs a
# statement once, then "moves to another backend": the cached id gets 1243 until it is prepared
# again.
# Usage: mysql_stmt_1243_test.sh <lk>   (needs python3)
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-33121}"
T="$(mktemp -d)"; s=""; trap '[ -n "$s" ] && kill $s 2>/dev/null; rm -rf "$T"' EXIT
fail=0
for mode in e1243 e1243first; do   # e1243first: 1243 hazırlamadan hemen sonraki EXECUTE'ta (ayna yolu)
[ -n "$s" ] && { kill $s 2>/dev/null; wait $s 2>/dev/null; }
python3 "$HERE/fake_mysql_server.py" $mode "$PORT" > "$T/my.log" 2>&1 & s=$!
for i in $(seq 1 50); do (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break; sleep 0.1; done
cat > "$T/q.lk" <<LK
use db
\$c = db::connect("mysql://u:p@127.0.0.1:$PORT/d")
\$a = db::query(\$c, "SELECT 'SECOND'", [])
\$b = db::query(\$c, "SELECT 'SECOND'", [])
\$e = db::query(\$c, "SELECT 'SECOND'", [])
\$d = db::query(\$c, "SELECT 'THIRD'", [])
print(json::encode([\$a[0]["col"], \$b[0]["col"], \$e[0]["col"], \$d[0]["col"]]))
LK
out="$(cd "$T" && timeout 60 "$LK" q.lk 2>&1 | grep -v '^\[20' | tail -1)"
[ "$out" = '["second","second","second","third"]' ] && echo "  OK   [$mode] $out" || { echo "  FAIL [$mode] $out"; fail=1; }
done
[ $fail = 0 ] && echo "PASS: error 1243 makes the statement be prepared again and run once" \
    || { echo "FAIL: statement cache after 1243"; exit 1; }
