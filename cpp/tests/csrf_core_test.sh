#!/usr/bin/env bash
# csrf:: and request::scheme()/request::host() — CSRF protection without server-side state.
#
# Every LOOK application had to write its own CSRF protection; LookPress kept the token in the
# session, so every visitor who saw a form opened a session file on the server (requests without a
# cookie could fill the disk). And an application that read X-Forwarded-Proto / X-Forwarded-Host
# itself let any client choose the scheme and host it was checked against.
#
# Now: csrf::token() / csrf::field() / csrf::verify() — a signed double-submit cookie
# (LOOK_SECRET, LOOK_SECRET_PREVIOUS while it changes); session::regenerate / session::destroy give
# a new token base. request::scheme() / request::host() apply the trusted-proxy rule
# (LOOK_TRUSTED_PROXY), as request::ip() does; CGI and FastCGI take them from the web server.
# Usage: csrf_core_test.sh <lk-fcgi> <lk-cgi>   (needs curl, python3)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
CGI="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
P=7651; FP=7652
S1=s1-0123456789abcdef0123456789abcdef; S2=s2-fedcba9876543210fedcba9876543210
T=$(mktemp -d); pid=""
trap '{ kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
BASE=http://127.0.0.1:$P
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
is()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: got '$2', want '$3'"; fi; }

cat > "$T/app.lk" <<'EOF'
route("GET", "/form", fn() => response::html(csrf::field()))
route("GET", "/tok", fn() => response::text(csrf::token()))
route("POST", "/submit", function() {
    if (!csrf::verify()) { return response::error(403, "csrf"); }
    return response::text("ok");
})
route("GET", "/login", function() { session::start(); session::regenerate(); return response::text("in"); })
route("GET", "/logout", function() { session::start(); session::destroy(); return response::text("out"); })
route("GET", "/plain", function() { session::start(); session::regenerate(); return response::text("p"); })
route("GET", "/info", fn() => response::text(request::scheme() . "|" . request::host()))
EOF

start() {   # start [env...] — --mode http on $P
    { kill $pid; wait $pid; } 2>/dev/null
    ( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SESSION_DIR="$T/s" "$@" "$LKF" --mode http --port $P --workers 2 app.lk >"$T/log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "$BASE/info" && return 0
        kill -0 $pid 2>/dev/null || return 1; sleep 0.2; done; return 1
}
get()   { curl -s -D "$T/h" -o "$T/b" --max-time 5 "$@"; }                       # headers → $T/h, body → $T/b
ck()    { grep -i "^set-cookie: $1=" "$T/h" | tail -1 | sed 's/^[^:]*: //; s/;.*//' | tr -d '\r'; }
ckline(){ grep -i "^set-cookie: $1=" "$T/h" | tail -1 | tr -d '\r'; }
tok()   { grep -o 'name="_csrf" value="[^"]*"' "$T/b" | sed 's/.*value="//; s/"$//'; }
post()  {   # post <cookie or ""> <token or ""> [curl args] → status of POST /submit
    local c="$1" t="$2"; shift 2
    local a=(-s -o /dev/null -w '%{http_code}' --max-time 5 -X POST -d x=1)
    [ -n "$c" ] && a+=(-H "Cookie: $c"); [ -n "$t" ] && a+=(--data-urlencode "_csrf=$t")
    curl "${a[@]}" "$@" "$BASE/submit"
}

echo "== CGI over HTTPS: the session cookie is Secure"
# (Uses only session::, so it also runs on releases without csrf::.) CGI never read HTTPS: on an
# HTTPS site the session cookie went out without Secure.
printf 'route("GET", "/s", function() { session::start(); return response::text("s"); })\n' > "$T/sess.lk"
sc=$(cd "$T" && env -i PATH=/usr/bin:/bin REQUEST_METHOD=GET SCRIPT_FILENAME="$T/sess.lk" PATH_INFO=/s REQUEST_URI=/s \
     GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" HTTPS=on HTTP_HOST=site.example "$CGI" 2>/dev/null | tr -d '\r' | grep -i '^set-cookie: LOOK_SESSION=')
[[ "$sc" == *Secure* ]] && ok "CGI with HTTPS=on: the session cookie has Secure" || bad "CGI with HTTPS=on: session cookie without Secure: $sc"

echo "== the token"
start LOOK_SECRET=$S1 || { echo "FAIL: did not start"; cat "$T/log"; exit 1; }
get "$BASE/form"; C=$(ck look_csrf); K=$(tok)
[ -n "$C" ] && [ -n "$K" ] && ok "csrf::field() sets the look_csrf cookie and gives a token" || bad "no cookie or token: '$C' '$K'"
L=$(ckline look_csrf)
[[ "$L" == *"Path=/"* && "$L" == *HttpOnly* && "$L" == *"SameSite=Lax"* && "$L" != *Secure* ]] && ok "cookie: Path=/, HttpOnly, SameSite=Lax, no Secure over http" || bad "cookie attributes: $L"
is "the form's token is accepted"                        "$(post "$C" "$K")" 200
is "X-CSRF-Token header is accepted"                     "$(post "$C" "" -H "X-CSRF-Token: $K")" 200
is "no cookie: refused"                                  "$(post "" "$K")" 403
is "no token: refused"                                   "$(post "$C" "")" 403
get -H "Cookie: $C" "$BASE/tok"; K2=$(cat "$T/b")
[ -z "$(ck look_csrf)" ] && ok "a second token reuses the cookie (no new Set-Cookie)" || bad "a second token set a new cookie"
[ "$K2" != "$K" ] && ok "every token is different (fresh nonce)" || bad "the same token twice"
is "the second token is accepted too"                    "$(post "$C" "$K2")" 200
get "$BASE/form"; C3=$(ck look_csrf); K3=$(tok)
is "another visitor's token: refused"                    "$(post "$C" "$K3")" 403
bad_sig="${C%?}$( [ "${C: -1}" = 0 ] && echo 1 || echo 0 )"
is "a cookie with a changed signature: refused"          "$(post "$bad_sig" "$K")" 403
forged_base=$(printf 'aa'); forged="look_csrf=$forged_base.$(printf 'csrf|%s' "$forged_base" | openssl dgst -sha256 -hmac wrong-key -hex 2>/dev/null | sed 's/.*= //')"
is "a cookie signed with another key: refused"           "$(post "$forged" "$K")" 403
echo "== Origin"
is "Origin of another site: refused"                     "$(post "$C" "$K" -H "Origin: http://evil.example")" 403
is "Origin of this site: accepted"                       "$(post "$C" "$K" -H "Origin: http://127.0.0.1:$P")" 200
is "Origin: null (sandbox, data:): refused"              "$(post "$C" "$K" -H "Origin: null")" 403
is "X-Forwarded-Host from a client is not trusted"       "$(post "$C" "$K" -H "Origin: http://evil.example" -H "X-Forwarded-Host: evil.example")" 403
echo "== request::scheme / request::host"
get "$BASE/info";                                        is "plain request"                                    "$(cat "$T/b")" "http|127.0.0.1:$P"
get -H "X-Forwarded-Proto: https" -H "X-Forwarded-Host: x.example" "$BASE/info"
is "X-Forwarded-* from an untrusted client is ignored"   "$(cat "$T/b")" "http|127.0.0.1:$P"
get -H "Host: Shop.Example.COM:8080" "$BASE/info";       is "host is lower case, port kept"                    "$(cat "$T/b")" "http|shop.example.com:8080"
get -H "Host: evil.example/x" "$BASE/info";              is "a host with a path is not a host"                 "$(cat "$T/b")" "http|"
get -H "Host: a@b" "$BASE/info";                         is "a host with @ is not a host"                      "$(cat "$T/b")" "http|"
echo "== a new token base at login and logout"
get -H "Cookie: $C" "$BASE/login"; CL=$(ck look_csrf)
[ -n "$CL" ] && [ "$CL" != "$C" ] && ok "session::regenerate gives a new token base" || bad "no new csrf cookie at login"
is "a token from before login is refused after login"    "$(post "$CL" "$K")" 403
get -H "Cookie: $CL" "$BASE/form"; KL=$(tok)
is "a token shown after login works"                     "$(post "$CL" "$KL")" 200
get -H "Cookie: $CL" "$BASE/logout"; CO=$(ck look_csrf)
[ -n "$CO" ] && [ "$CO" != "$CL" ] && ok "session::destroy gives a new token base" || bad "no new csrf cookie at logout"
get "$BASE/plain"
[ -z "$(ck look_csrf)" ] && ok "an app without csrf:: gets no csrf cookie from session::regenerate" || bad "regenerate set a csrf cookie without csrf::"

echo "== changing LOOK_SECRET"
start LOOK_SECRET=$S2 LOOK_SECRET_PREVIOUS=$S1 || { echo "FAIL: did not start"; exit 1; }
is "with LOOK_SECRET_PREVIOUS: an open form still works" "$(post "$C" "$K")" 200
get -H "Cookie: $C" "$BASE/tok"; CN=$(ck look_csrf); KN=$(cat "$T/b")
[ -n "$CN" ] && [ "$CN" != "$C" ] && [ "${CN%%.*}" = "${C%%.*}" ] && ok "the cookie is signed again with the new key (same base)" || bad "not re-signed: '$CN'"
is "the re-signed cookie works"                          "$(post "$CN" "$KN")" 200
start LOOK_SECRET=$S2 || { echo "FAIL: did not start"; exit 1; }
is "without LOOK_SECRET_PREVIOUS: the old cookie is refused" "$(post "$C" "$K")" 403
is "the re-signed cookie still works"                    "$(post "$CN" "$KN")" 200

echo "== LOOK_SECRET is required"
start || { echo "FAIL: did not start without LOOK_SECRET"; exit 1; }
get "$BASE/form"; is "no LOOK_SECRET: csrf::field() fails (500)" "$(head -1 "$T/h" | awk '{print $2}')" 500
grep -q "LOOK_SECRET is not set or shorter than 32 characters" "$T/log" && ok "and the log says why" || bad "no reason in the log"
get "$BASE/plain"; is "an app without csrf:: does not need LOOK_SECRET" "$(head -1 "$T/h" | awk '{print $2}')" 200
start LOOK_SECRET=short || { echo "FAIL: did not start"; exit 1; }
get "$BASE/form"; is "a LOOK_SECRET shorter than 32 characters fails" "$(head -1 "$T/h" | awk '{print $2}')" 500

echo "== behind a trusted proxy"
start LOOK_SECRET=$S1 LOOK_TRUSTED_PROXY=127.0.0.1 || { echo "FAIL: did not start"; exit 1; }
PX=(-H "X-Forwarded-Proto: https" -H "X-Forwarded-Host: Shop.Example.com")
get "${PX[@]}" "$BASE/info";                             is "scheme and host from the trusted proxy" "$(cat "$T/b")" "https|shop.example.com"
get "${PX[@]}" "$BASE/form"; CH=$(ck __Host-look_csrf); KH=$(tok); LH=$(ckline __Host-look_csrf)
[ -n "$CH" ] && [[ "$LH" == *Secure* && "$LH" == *"Path=/"* && "$LH" != *Domain* ]] && ok "over https: __Host-look_csrf, Secure, Path=/, no Domain" || bad "https cookie: $LH"
is "Origin = the forwarded host: accepted"               "$(post "$CH" "$KH" "${PX[@]}" -H "Origin: https://shop.example.com")" 200
is "Origin of another site: refused"                     "$(post "$CH" "$KH" "${PX[@]}" -H "Origin: https://evil.example")" 403

echo "== the discarded result of csrf::verify()"
printf 'route("POST", "/x", function() { csrf::verify(); return response::text("x"); })\n' > "$T/bad.lk"
out=$(cd "$T" && LOOK_SECRET=$S1 timeout 5 "$LKF" --mode http --port 7653 bad.lk 2>&1); rc=$?
[ $rc != 0 ] && [ $rc != 124 ] && [[ "$out" == *csrf::verify* ]] && ok "\"csrf::verify();\" without using the result: LOOK does not start and names it" \
    || bad "a discarded csrf::verify() was accepted (rc=$rc): $(echo "$out" | tail -2)"

echo "== FastCGI and CGI take scheme and host from the web server"
{ kill $pid; wait $pid; } 2>/dev/null
( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET=$S1 "$LKF" --port $FP --workers 2 --script "$T/app.lk" >"$T/flog" 2>&1 ) & pid=$!
cat > "$T/fcgi.py" <<'EOF'
import socket, struct, sys
def rec(t, c): return struct.pack(">BBHHBx", 1, t, 1, len(c), 0) + c
def ln(n): return bytes([n]) if n < 128 else struct.pack(">I", n | 0x80000000)
def nv(k, v): k, v = k.encode(), v.encode(); return ln(len(k)) + ln(len(v)) + k + v
p = dict(a.split("=", 1) for a in sys.argv[2:])
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(rec(1, struct.pack(">HB5x", 1, 0)) + rec(4, b"".join(nv(k, v) for k, v in p.items())) + rec(4, b"") + rec(5, b""))
buf, out = b"", b""
while True:
    d = s.recv(65536)
    if not d: break
    buf += d
    while len(buf) >= 8:
        _, t, _, cl, pl = struct.unpack(">BBHHB", buf[:7])
        if len(buf) < 8 + cl + pl: break
        if t == 6: out += buf[8:8 + cl]
        buf = buf[8 + cl + pl:]
        if t == 3: sys.stdout.write(out.decode(errors="replace").split("\r\n\r\n", 1)[-1]); sys.exit(0)
EOF
for i in $(seq 1 50); do python3 "$T/fcgi.py" $FP REQUEST_METHOD=GET REQUEST_URI=/info SCRIPT_FILENAME="$T/app.lk" >/dev/null 2>&1 && break; sleep 0.2; done
FC=(REQUEST_METHOD=GET REQUEST_URI=/info SCRIPT_FILENAME="$T/app.lk")
is "FastCGI: HTTPS=on and HTTP_HOST"                     "$(python3 "$T/fcgi.py" $FP "${FC[@]}" HTTPS=on HTTP_HOST=Site.Example:8443)" "https|site.example:8443"
is "FastCGI: X-Forwarded-Host from the client is ignored" "$(python3 "$T/fcgi.py" $FP "${FC[@]}" HTTP_HOST=site.example HTTP_X_FORWARDED_HOST=evil.example HTTP_X_FORWARDED_PROTO=https)" "http|site.example"
cg() { (cd "$T" && env -i PATH=/usr/bin:/bin REQUEST_METHOD=GET SCRIPT_FILENAME="$T/app.lk" PATH_INFO=/info REQUEST_URI=/info \
        GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" "$@" "$CGI" 2>/dev/null) | tr -d '\r' | sed '1,/^$/d'; }
is "CGI: HTTPS=on and HTTP_HOST (CGI never set https before)" "$(cg HTTPS=on HTTP_HOST=Site.Example)" "https|site.example"
is "CGI: plain http"                                      "$(cg HTTP_HOST=site.example)" "http|site.example"

[ $fail = 0 ] && echo "PASS: csrf:: and request::scheme/host" || echo "FAIL: csrf:: and request::scheme/host"
exit $fail
