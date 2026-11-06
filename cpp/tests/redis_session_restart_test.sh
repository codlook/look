#!/usr/bin/env bash
# Redis sessions keep working after Redis closes the idle connections (restart, `timeout`
# setting, CLIENT KILL): the next request opens a new connection instead of failing.
#
# Session connections wait in a pool between requests. A connection Redis had closed was taken
# from the pool and used: the first command after a Redis restart failed and the request got 503
# (one per pooled connection). Now a pooled connection is checked before anything is sent — the
# same check MySQL and PostgreSQL connections get — and a closed one is replaced. No command is
# ever sent twice.
# The test closes every client connection with CLIENT KILL (what a restart does to them).
# Usage: redis_session_restart_test.sh <lk-fcgi> <redis-url>   (needs curl)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
URL="${2:?usage}"
PORT="${PORT:-7466}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }
hp="${URL#*://}"; hp="${hp##*@}"; hp="${hp%%/*}"; RH="${hp%%:*}"; RP="${hp##*:}"; [ "$RP" = "$hp" ] && RP=6379

kill_clients() {   # CLIENT KILL TYPE normal SKIPME yes → bizim dışımızdaki her istemci kapanır
    exec 3<>"/dev/tcp/$RH/$RP" || return 1
    printf '*6\r\n$6\r\nCLIENT\r\n$4\r\nKILL\r\n$4\r\nTYPE\r\n$6\r\nnormal\r\n$6\r\nSKIPME\r\n$3\r\nyes\r\n' >&3
    read -r -t 2 line <&3; exec 3<&- 3>&-
    echo "$line" | tr -d '\r'
}

cat > "$T/app.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", request::get("v")); return response::text("ok") })
route("GET", "/get", function() { session::start(); return response::text(session::get("v") ?? "none") })
route("GET", "/ping", fn() => response::text("ok"))
LK
export LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="$URL"
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -m 5 -o /dev/null -m 1 "localhost:$PORT/ping" && break; sleep 0.1; done
U="http://localhost:$PORT"

cp=""; for i in $(seq 1 8); do curl -s -m 5 -c "$T/j$i" "$U/set?v=user$i" > /dev/null & cp="$cp $!"; done
wait $cp   # havuzda birkaç bağlantı (çıplak wait sunucuyu da beklerdi)
for i in $(seq 1 8); do [ "$(curl -s -m 5 -b "$T/j$i" "$U/get")" = "user$i" ] || bad "before: client $i lost its session"; done
k="$(kill_clients)"; echo "  ..   CLIENT KILL: ${k:-no reply}"
sleep 0.3
codes=""; vals=0
for i in $(seq 1 8); do
    r="$(curl -s -m 5 -w ' %{http_code}' -b "$T/j$i" "$U/get")"
    codes="$codes ${r##* }"; [ "${r% *}" = "user$i" ] && vals=$((vals + 1))
done
echo "$codes" | grep -q 503 && bad "requests after Redis closed the connections got 503:$codes" \
    || ok "no request failed after Redis closed the pooled connections"
[ $vals = 8 ] && ok "every client still reads its own session" || bad "only $vals of 8 clients read their session"

[ $fail = 0 ] && echo "PASS: pooled Redis session connections closed by Redis are replaced before use" \
             || { echo "FAIL: Redis session pool after Redis closed its connections"; exit 1; }
