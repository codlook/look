#!/usr/bin/env bash
# Behind a trusted proxy, request::ip(), request::host() and request::scheme() take the proxy's
# values, never the ones a client wrote into the same headers.
#
# From a LOOK_TRUSTED_PROXY address, X-Real-IP won and then the LEFTMOST X-Forwarded-For entry;
# X-Forwarded-Host / -Proto took the leftmost value too. Behind a proxy that appends (Apache
# mod_proxy, nginx $proxy_add_x_forwarded_for) the leftmost entry is the client's own: a client
# chose its IP (rate limits, bans, audit log) and request::host() (password-reset links). Now
# X-Forwarded-For is read from the right, skipping trusted proxies; X-Real-IP only when there is
# no X-Forwarded-For; the forwarded host and scheme are the rightmost values.
# Usage: proxy_trust_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7757}"
cat > "$TMP/app.lk" <<'LK'
route("GET", "/w", fn() => response::text(request::ip() . "|" . request::host() . "|" . request::scheme()))
LK
( cd "$TMP" && exec env LOOK_TRUSTED_PROXY=127.0.0.1 "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "http://127.0.0.1:$PORT/w" && break; sleep 0.1; done
fail=0
t() {   # name, want, curl args...
    local name="$1" want="$2"; shift 2
    local got; got="$(curl -s -m 5 "$@" "http://127.0.0.1:$PORT/w")"
    if [ "$got" = "$want" ]; then echo "  OK   $name"; else echo "  FAIL $name: got '$got', want '$want'"; fail=1; fi
}
H="Host: real.example"
t "appended X-Forwarded-For: the proxy's entry, not the client's"  "203.0.113.7|real.example|http" -H "$H" -H "X-Forwarded-For: 6.6.6.6, 203.0.113.7"
t "X-Real-IP from the client is ignored when X-Forwarded-For is present" "203.0.113.7|real.example|http" -H "$H" -H "X-Real-IP: 6.6.6.6" -H "X-Forwarded-For: 203.0.113.7"
t "a chain through another trusted proxy: the first untrusted from the right" "203.0.113.7|real.example|http" -H "$H" -H "X-Forwarded-For: 6.6.6.6, 203.0.113.7, 127.0.0.1"
t "X-Real-IP alone (nginx without X-Forwarded-For)"                "198.51.100.4|real.example|http" -H "$H" -H "X-Real-IP: 198.51.100.4"
t "a malformed entry is not taken as the client"                   "127.0.0.1|real.example|http" -H "$H" -H "X-Forwarded-For: 6.6.6.6, <script>"
t "appended X-Forwarded-Host: the proxy's value"                   "127.0.0.1|real.example|http" -H "Host: x" -H "X-Forwarded-Host: evil.example, real.example"
t "appended X-Forwarded-Proto: the proxy's value"                  "127.0.0.1|real.example|http" -H "$H" -H "X-Forwarded-Proto: https, http"
t "the proxy's https is honoured"                                  "127.0.0.1|real.example|https" -H "$H" -H "X-Forwarded-Proto: https"
# A peer that is not a trusted proxy: its forwarding headers are ignored.
if curl -s -m 2 --interface 127.0.0.5 -o /dev/null "http://127.0.0.1:$PORT/w" 2>/dev/null; then
    t "an untrusted peer cannot set any of them" "127.0.0.5|real.example|http" --interface 127.0.0.5 -H "$H" \
      -H "X-Forwarded-For: 6.6.6.6" -H "X-Real-IP: 6.6.6.6" -H "X-Forwarded-Host: evil.example" -H "X-Forwarded-Proto: https"
fi
kill -0 "$pid" 2>/dev/null || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: forwarding headers from a trusted proxy are read from the proxy's end" \
             || { echo "FAIL: forwarding header trust"; exit 1; }
