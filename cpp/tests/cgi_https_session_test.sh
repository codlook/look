#!/usr/bin/env bash
# Under CGI over HTTPS the session cookie is Secure.
#
# lk-cgi never read the web server's HTTPS variable: on an HTTPS site the session cookie went out
# without Secure, and every request logged a warning that pointed at a reverse proxy. FastCGI and
# --mode http already followed the connection. Now CGI takes HTTPS / REQUEST_SCHEME from the web
# server, as FastCGI does (the client cannot set these).
# Usage: cgi_https_session_test.sh <lk-cgi>
set -u
CGI="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -m 700 "$T/s"
printf 'route("GET", "/s", function() { session::start(); return response::text("s"); })\n' > "$T/app.lk"
fail=0
run() {   # run [env...] → the response headers and stderr
    (cd "$T" && env -i PATH=/usr/bin:/bin REQUEST_METHOD=GET SCRIPT_FILENAME="$T/app.lk" PATH_INFO=/s REQUEST_URI=/s \
        GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" "$@" "$CGI" 2>&1) | tr -d '\r'
}
check() {   # check <name> <want Secure: yes|no> [env...]
    local name="$1" want="$2"; shift 2
    local out ck; out=$(run "$@"); ck=$(echo "$out" | grep -i '^set-cookie: LOOK_SESSION=')
    [ -n "$ck" ] || { echo "  FAIL $name: no session cookie"; fail=1; return; }
    local has=no; [[ "$ck" == *Secure* ]] && has=yes
    if [ "$has" = "$want" ]; then echo "  OK   $name"; else echo "  FAIL $name: $ck"; fail=1; fi
}
check "HTTPS=on: Secure"                      yes HTTPS=on
check "HTTPS=1: Secure"                       yes HTTPS=1
check "REQUEST_SCHEME=https: Secure"          yes REQUEST_SCHEME=https
check "plain http: no Secure"                 no
check "HTTPS=off: no Secure"                  no  HTTPS=off
check "LOOK_SESSION_SECURE=0 over HTTPS: no Secure" no HTTPS=on LOOK_SESSION_SECURE=0
out=$(run HTTPS=on)
[[ "$out" != *"WITHOUT Secure"* ]] && echo "  OK   HTTPS=on: no 'without Secure' warning" || { echo "  FAIL HTTPS=on still warns"; fail=1; }
[ $fail = 0 ] && echo "PASS: CGI session cookie follows HTTPS" || echo "FAIL: CGI session cookie and HTTPS"
exit $fail
