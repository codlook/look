#!/usr/bin/env bash
# A script that used MySQL leaves no aborted connection behind.
#
# The driver closed its sockets without COM_QUIT: MySQL counted every connection as aborted
# (Aborted_clients, ER_NET_READ_ERROR) and wrote a warning to its error log — a script making
# one query aborted all of its pool's connections (16 on a 16-core machine).
# Needs a live MySQL server and an account that may read global status (CI: mysql:8 service).
# Usage: mysql_quit_test.sh <lk> <mysql-dsn>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <dsn>}")" && pwd)/$(basename "$1")"
DSN="${2:?usage: $0 <lk> <dsn>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

printf 'use db\n$c = db::connect(env("MY_DSN"))\nprint(db::col($c, "SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = \x27Aborted_clients\x27", []))\n' > "$TMP/count.lk"
printf 'use db\n$c = db::connect(env("MY_DSN"))\nprint(db::col($c, "SELECT 1", []))\n' > "$TMP/one.lk"
count() { MY_DSN="$DSN" "$LK" "$TMP/count.lk" 2>/dev/null | tail -1; }

before="$(count)"
for i in 1 2 3; do MY_DSN="$DSN" "$LK" "$TMP/one.lk" >/dev/null 2>&1; done
after="$(count)"
# The first count() run itself closes its connections too: only scripts run between the two
# reads can change the number (plus the first count script's own connections).
if [ -z "$before" ] || [ -z "$after" ]; then echo "FAIL: could not read Aborted_clients (before='$before' after='$after')"; exit 1; fi
if [ "$after" = "$before" ]; then echo "  OK   4 scripts, Aborted_clients stayed at $before"; echo "PASS: MySQL connections are closed with COM_QUIT"; exit 0; fi
echo "  FAIL Aborted_clients went from $before to $after"; echo "FAIL: MySQL connections are cut without COM_QUIT"; exit 1
