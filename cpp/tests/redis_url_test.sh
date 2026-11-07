#!/usr/bin/env bash
# LOOK_REDIS_URL address handling (go-redis ParseURL model), with Redis sessions on a live server:
#   * a bad port or database number is an error that names it (the request gets 503 and the log
#     says why) — "6379x" used to connect to 6379, "/abc" silently used database 0 (another
#     application's keys), ":99999" was accepted;
#   * "/2" uses database 2;
#   * an IPv6 address in brackets ("redis://[::1]:6379") connects — it used to be split at the
#     last ':' into the host "[:".
# Usage: redis_url_test.sh <lk-fcgi> <redis-url on 127.0.0.1>   (needs curl; Redis must also
#        listen on ::1 for the IPv6 row — the stock image and Docker's published ports do)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
URL="${2:?usage}"
PORT="${PORT:-7798}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
hp="${URL#*://}"; hp="${hp##*@}"; hp="${hp%%/*}"; RH="${hp%%:*}"; RP="${hp##*:}"; [ "$RP" = "$hp" ] && RP=6379
fail=0
cat > "$T/app.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", "x"); return response::text("ok") })
route("GET", "/ping", fn() => response::text("ok"))
LK
start() {   # url
    [ -n "$pid" ] && { kill $pid 2>/dev/null; wait $pid 2>/dev/null; }
    ( cd "$T" && LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="$1" exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ping" && break; sleep 0.1; done
}
dbsize() {   # db → anahtar sayısı
    exec 3<>"/dev/tcp/$RH/$RP" || { echo "?"; return; }
    printf '*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n*1\r\n$6\r\nDBSIZE\r\n' "${#1}" "$1" >&3
    read -r -t 2 a <&3; read -r -t 2 b <&3; exec 3<&- 3>&-
    echo "$b" | tr -d '\r:'
}
ok_url() {   # ad url
    start "$2"
    c="$(curl -s -m 5 -o /dev/null -w '%{http_code}' "localhost:$PORT/set")"
    [ "$c" = 200 ] && echo "  OK   $1" || { echo "  FAIL $1: HTTP $c; $(tail -2 "$T/log.txt")"; fail=1; }
}
bad_url() {   # ad url beklenen-günlük-metni
    start "$2"
    c="$(curl -s -m 5 -o /dev/null -w '%{http_code}' "localhost:$PORT/set")"
    if [ "$c" = 503 ] && grep -qF "$3" "$T/log.txt"; then echo "  OK   $1"
    else echo "  FAIL $1: HTTP $c, want 503 and \"$3\" in the log: $(tail -2 "$T/log.txt")"; fail=1; fi
}
B="redis://$RH:$RP"
ok_url  "valid URL"               "$B"
bad_url "port ${RP}x"             "redis://$RH:${RP}x"   "invalid port \"${RP}x\""
bad_url "port 99999"              "redis://$RH:99999"    'invalid port "99999"'
bad_url "database abc"            "$B/abc"               'invalid database number "abc"'
bad_url "IPv6 without brackets"   "redis://::1:$RP"      'must be in brackets'
before=$(dbsize 2)
ok_url  "database 2"              "$B/2"
after=$(dbsize 2)
[ "$after" != "?" ] && [ "$after" -gt "${before:-0}" ] && echo "  OK   the session went to database 2 ($before -> $after keys)" \
    || { echo "  FAIL database 2 holds $before -> $after keys"; fail=1; }
if python3 -c "import socket; socket.create_connection(('::1', $RP), timeout=2).close()" 2>/dev/null; then
    ok_url "IPv6 in brackets"     "redis://[::1]:$RP"
elif [ -n "${CI:-}" ]; then echo "  FAIL ::1 is not reachable on port $RP"; fail=1
else echo "  SKIP IPv6 row: ::1 is not reachable on port $RP"; fi
[ $fail = 0 ] && echo "PASS: LOOK_REDIS_URL address handling" || { echo "FAIL: LOOK_REDIS_URL address handling"; exit 1; }
