#!/usr/bin/env bash
# db::connect address handling against real MySQL and PostgreSQL servers (Go net/url model):
#   * a bad port is an error that names the port — "3306x" used to connect to 3306, "abc" gave a
#     bare "stoi", "99999" was accepted — and the message never contains the password;
#   * an empty port ("host:/db") means the default port;
#   * an IPv6 address in brackets ("[::1]:3306") connects — it used to be split at the first ':'
#     and the clients only resolved IPv4;
#   * a query string without a path ("host:3306?tls=insecure") keeps its options — the query used
#     to be read as part of the port, so ?tls was silently lost.
# Usage: db_dsn_address_test.sh <lk> <mysql_dsn> <pg_dsn>   (DSNs on 127.0.0.1; the servers must
#        also listen on ::1 — Docker's published ports and the stock images do)
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
MY="${2:?mysql dsn}"; PG="${3:?pg dsn}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fail=0

# DSN parçaları: şema://kimlik@127.0.0.1:port/db
my_auth="${MY#*://}"; my_auth="${my_auth%@*}"; my_rest="${MY##*@}"; my_port="${my_rest#*:}"; my_port="${my_port%%/*}"; my_db="${my_rest#*/}"
pg_auth="${PG#*://}"; pg_auth="${pg_auth%@*}"; pg_rest="${PG##*@}"; pg_port="${pg_rest#*:}"; pg_port="${pg_port%%/*}"; pg_db="${pg_rest#*/}"
my_pass="${my_auth#*:}"

# ::1 erişilebilir mi (IPv6 kapalı makine): CI'da kırmızı, yerelde atla
v6=1
python3 -c "import socket; socket.create_connection(('::1', $my_port), timeout=2).close(); socket.create_connection(('::1', $pg_port), timeout=2).close()" 2>/dev/null || v6=0
if [ $v6 = 0 ]; then
    if [ -n "${CI:-}" ]; then echo "  FAIL ::1 is not reachable on ports $my_port/$pg_port"; fail=1
    else echo "  SKIP IPv6 rows: ::1 is not reachable on ports $my_port/$pg_port"; fi
fi

ok_case() {   # ad dsn
    cat > "$T/q.lk" <<LK
use db
\$c = db::connect("$2")
\$r = db::query(\$c, "SELECT 1 AS one")
print("one=" . \$r[0]["one"])
LK
    out="$("$LK" "$T/q.lk" 2>&1 | grep -v '^\[20')"
    if echo "$out" | grep -q "^one=1$"; then echo "  OK   $1"; else echo "  FAIL $1: $out"; fail=1; fi
}
err_case() {   # ad dsn beklenen-metin
    cat > "$T/q.lk" <<LK
use db
try { \$c = db::connect("$2"); print("CONNECTED") }
catch (\$e) { print("ERR " . \$e) }
LK
    out="$("$LK" "$T/q.lk" 2>&1 | grep -v '^\[20')"
    if echo "$out" | grep -qF "$3" && ! echo "$out" | grep -qF "$my_pass"; then echo "  OK   $1"
    else echo "  FAIL $1: want \"$3\" (and no password), got: $out"; fail=1; fi
}

err_case "mysql port 3306x"        "mysql://$my_auth@127.0.0.1:${my_port}x/$my_db"     "invalid port \"${my_port}x\""
err_case "mysql port abc"          "mysql://$my_auth@127.0.0.1:abc/$my_db"             'invalid port "abc"'
err_case "mysql port 99999"        "mysql://$my_auth@127.0.0.1:99999/$my_db"           'invalid port "99999"'
err_case "mysql unclosed ["        "mysql://$my_auth@[::1:$my_port/$my_db"             "missing ']'"
err_case "mysql IPv6 no brackets"  "mysql://$my_auth@::1:$my_port/$my_db"              "must be in brackets"
err_case "pg port 5432x"           "postgres://$pg_auth@127.0.0.1:${pg_port}x/$pg_db"  "invalid port \"${pg_port}x\""
ok_case  "mysql 127.0.0.1"         "$MY"
ok_case  "pg 127.0.0.1"            "$PG"
# Sorgu yolsuz: bağlanması yetmez, ?tls gerçekten uygulanmış olmalı (Ssl_cipher dolu)
cat > "$T/q.lk" <<LK
use db
\$c = db::connect("mysql://$my_auth@127.0.0.1:$my_port?tls=insecure")
\$r = db::query(\$c, "SHOW SESSION STATUS LIKE 'Ssl_cipher'")
print("cipher=" . \$r[0]["Value"])
LK
out="$("$LK" "$T/q.lk" 2>&1 | grep -v '^\[20')"
if echo "$out" | grep -qE "^cipher=.+"; then echo "  OK   mysql query without a path keeps ?tls ($out)"
else echo "  FAIL mysql query without a path keeps ?tls: $out"; fail=1; fi
if [ "$my_port" = 3306 ]; then ok_case "mysql empty port = 3306" "mysql://$my_auth@127.0.0.1:/$my_db"; fi
if [ "$pg_port" = 5432 ]; then ok_case "pg empty port = 5432"    "postgres://$pg_auth@127.0.0.1:/$pg_db"; fi
if [ $v6 = 1 ]; then
    ok_case "mysql [::1]"          "mysql://$my_auth@[::1]:$my_port/$my_db"
    ok_case "pg [::1]"             "postgres://$pg_auth@[::1]:$pg_port/$pg_db"
fi
[ $fail = 0 ] && echo "PASS: db::connect address handling" || { echo "FAIL: db::connect address handling"; exit 1; }
