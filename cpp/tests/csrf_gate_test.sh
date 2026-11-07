#!/usr/bin/env bash
# The default CSRF gate (Go 1.25 net/http CrossOriginProtection model):
#   * every method other than GET, HEAD and OPTIONS needs a token — route("PROPFIND"/"PURGE", ...)
#     used to skip the gate, which only listed POST/PUT/PATCH/DELETE;
#   * Sec-Fetch-Site: cross-site / same-site is refused even with a token; same-origin and none
#     (the address bar) pass;
#   * an exempt prefix covers only its own routes: "/webhooks/../account" does not reach /account;
#   * the token works in a header, a urlencoded form and a multipart form.
# Usage: csrf_gate_test.sh <lk-fcgi> [port]
set -u
FCGI="$(cd "$(dirname "${1:?usage: csrf_gate_test.sh <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7921}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
csrf::exempt("/webhooks/*")
route("GET", "/form", fn() => response::text(csrf::token()))
route("POST", "/account", fn() => response::text("CHANGED"))
route("POST", "/webhooks/{x}", fn() => response::text("WEBHOOK"))
route("PROPFIND", "/dav", fn() => response::text("CHANGED"))
route("PURGE", "/cache", fn() => response::text("CHANGED"))
LK
( cd "$T" && LOOK_SECRET=csrf-gate-test-secret-0123456789abcdef01 exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/form" && break; sleep 0.1; done
B="http://localhost:$PORT"; J="$T/jar"
tok="$(curl -s -c "$J" "$B/form")"
fail=0
c() {   # ad beklenen-kod curl-argümanları...
    local name="$1" want="$2"; shift 2
    got="$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$@")"
    [ "$got" = "$want" ] && echo "  OK   $name ($got)" || { echo "  FAIL $name: want $want, got $got"; fail=1; }
}
c "POST without a token"                      403 -X POST -b "$J" "$B/account"
c "POST, token in a header"                   200 -X POST -b "$J" -H "X-CSRF-Token: $tok" "$B/account"
c "POST, token in a form"                     200 -X POST -b "$J" -d "_csrf=$tok" "$B/account"
c "POST, token in a multipart form"           200 -X POST -b "$J" -F "_csrf=$tok" -F "a=1" "$B/account"
c "PROPFIND without a token"                  403 -X PROPFIND -b "$J" "$B/dav"
c "PURGE without a token"                     403 -X PURGE "$B/cache"
c "PROPFIND with a token"                     200 -X PROPFIND -b "$J" -H "X-CSRF-Token: $tok" "$B/dav"
c "Sec-Fetch-Site: same-origin + token"       200 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Sec-Fetch-Site: same-origin" "$B/account"
c "Sec-Fetch-Site: none + token"              200 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Sec-Fetch-Site: none" "$B/account"
c "Sec-Fetch-Site: cross-site + token"        403 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Sec-Fetch-Site: cross-site" "$B/account"
c "Sec-Fetch-Site: same-site + token"         403 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Sec-Fetch-Site: same-site" "$B/account"
c "Origin of another site + token"            403 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Origin: https://evil.example" "$B/account"
c "Origin of this site + token"               200 -X POST -b "$J" -H "X-CSRF-Token: $tok" -H "Origin: http://localhost:$PORT" "$B/account"
c "exempt route"                              200 -X POST "$B/webhooks/stripe"
c "exempt prefix does not reach /account"     404 -X POST "$B/webhooks/../account"
c "exempt prefix, encoded dots"               404 -X POST "$B/webhooks/%2e%2e/account"
grep -q "Sec-Fetch-Site: cross-site" "$T/log.txt" && echo "  OK   the log says why (Sec-Fetch-Site)" || { echo "  FAIL the log does not name Sec-Fetch-Site"; fail=1; }
[ $fail = 0 ] && echo "PASS: CSRF gate covers every unsafe method and Sec-Fetch-Site" || { echo "FAIL: CSRF gate"; exit 1; }
