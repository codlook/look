#!/usr/bin/env bash
# CSRF protection is on by default: every application that uses cookies is protected.
#
# Every LOOK application had to write its own CSRF protection, and an application that forgot to
# check a POST route was open without a word. LookPress kept the token in the session, so every
# visitor who saw a form opened a session file on the server. And an application that read
# X-Forwarded-Proto / X-Forwarded-Host itself let any client choose the scheme and host.
#
# Now, when a program uses session::, csrf:: or cookie::, every POST, PUT, PATCH and DELETE is
# checked before before_route and the route run — in --mode http, FastCGI and CGI alike: the token
# comes from csrf::field() (a form field, also in multipart forms) or csrf::token() (an
# X-CSRF-Token header); its base is in a cookie signed with LOOK_SECRET, nothing is stored on the
# server. csrf::exempt() lets webhooks and token APIs through, and those may not use the session.
# Programs without cookies are not affected. Without LOOK_SECRET a program with cookies does not
# start. request::scheme()/host() follow the trusted-proxy rule; CGI takes HTTPS from the server.
# Usage: csrf_core_test.sh <lk-fcgi> <lk-cgi>   (needs curl, python3)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
CGI="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
P=7651; FP=7652
S1=s1-0123456789abcdef0123456789abcdef; S2=s2-fedcba9876543210fedcba9876543210
T=$(mktemp -d); pid=""; fpid=""
trap '{ kill $pid $fpid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
BASE=http://127.0.0.1:$P
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
is()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: got '$2', want '$3'"; fi; }
mkdir -m 700 "$T/s"

cat > "$T/app.lk" <<'EOF'
before_route(function() { response::header("X-Mw", "ran"); })
route("GET", "/form", fn() => response::html(csrf::field()))
route("GET", "/tok", fn() => response::text(csrf::token()))
route("POST", "/submit", fn() => response::text("ok"))
route("PUT", "/submit", fn() => response::text("ok"))
route("PATCH", "/submit", fn() => response::text("ok"))
route("DELETE", "/submit", fn() => response::text("ok"))
route("POST", "/upload", function() {
    $f = request::file("doc", ["allow_mime" => ["text/plain"]])
    return response::text($f == null ? "nofile" : "file")
})
route("POST", "/api/json", fn() => response::text("json:" . json::encode(request::json())))
csrf::exempt("/hook")
csrf::exempt("/token-api/*")
route("POST", "/hook", fn() => response::text("hook"))
route("POST", "/token-api/{id}", fn() => response::text("api"))
route("POST", "/hook-with-session", fn() => response::text("x"))
csrf::exempt("/hook-with-session")
route("POST", "/hook-session", function() { session::start(); return response::text("s"); })
csrf::exempt("/hook-session")
route("GET", "/login", function() { session::start(); session::regenerate(); return response::text("in"); })
route("GET", "/logout", function() { session::start(); session::destroy(); return response::text("out"); })
route("GET", "/info", fn() => response::text(request::scheme() . "|" . request::host()))
route("GET", "/late-exempt", function() { csrf::exempt("/x"); return response::text("x"); })
EOF
printf 'line one\n' > "$T/doc.txt"
B="----lkb0undary"
mpart() {   # mpart <token or ""> → multipart body file
    { [ -n "$1" ] && printf -- '--%s\r\nContent-Disposition: form-data; name="_csrf"\r\n\r\n%s\r\n' "$B" "$1"
      printf -- '--%s\r\nContent-Disposition: form-data; name="doc"; filename="doc.txt"\r\nContent-Type: text/plain\r\n\r\nline one\r\n--%s--\r\n' "$B" "$B"; } > "$T/mp"
}

# ── the same request in three modes ──────────────────────────────────────────────
# req <mode> <method> <path> <cookie> <x-csrf-token> <content-type> <body file|""> [origin]
#   → headers in $T/h, body in $T/b; prints the status code
cat > "$T/fcgi.py" <<'EOF'
import os, socket, struct, sys
def rec(t, c): return struct.pack(">BBHHBx", 1, t, 1, len(c), 0) + c
def ln(n): return bytes([n]) if n < 128 else struct.pack(">I", n | 0x80000000)
def nv(k, v): k, v = k.encode(), v.encode(); return ln(len(k)) + ln(len(v)) + k + v
p = dict(a.split("=", 1) for a in sys.argv[2:])
body = open(os.environ["BODY"], "rb").read() if os.environ.get("BODY") else b""
if body: p["CONTENT_LENGTH"] = str(len(body))
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
out = rec(1, struct.pack(">HB5x", 1, 0)) + rec(4, b"".join(nv(k, v) for k, v in p.items())) + rec(4, b"")
for i in range(0, len(body), 60000): out += rec(5, body[i:i + 60000])
s.sendall(out + rec(5, b""))
buf, res = b"", b""
while True:
    d = s.recv(65536)
    if not d: break
    buf += d
    while len(buf) >= 8:
        _, t, _, cl, pl = struct.unpack(">BBHHB", buf[:7])
        if len(buf) < 8 + cl + pl: break
        if t == 6: res += buf[8:8 + cl]
        buf = buf[8 + cl + pl:]
        if t == 3: sys.stdout.buffer.write(res); sys.exit(0)
EOF
split_raw() {   # $T/raw (CGI/FastCGI headers + body) → $T/h, $T/b; prints the status
    tr -d '\r' < "$T/raw" | sed '/^$/q' > "$T/h"
    tr -d '\r' < "$T/raw" | sed '1,/^$/d' > "$T/b"
    local st; st=$(grep -i '^status:' "$T/h" | head -1 | awk '{print $2}'); echo "${st:-200}"
}
req() {
    local mode="$1" m="$2" path="$3" ck="$4" xt="$5" ct="$6" bf="$7" org="${8:-}"
    case "$mode" in
    http)
        local a=(-s -D "$T/h" -o "$T/b" --max-time 5 -X "$m")
        [ -n "$ck" ] && a+=(-H "Cookie: $ck"); [ -n "$xt" ] && a+=(-H "X-CSRF-Token: $xt")
        [ -n "$ct" ] && a+=(-H "Content-Type: $ct"); [ -n "$bf" ] && a+=(--data-binary "@$bf")
        [ -n "$org" ] && a+=(-H "Origin: $org")
        curl "${a[@]}" "$BASE$path" >/dev/null; head -1 "$T/h" | awk '{print $2}' ;;
    fcgi)
        local pa=(REQUEST_METHOD="$m" REQUEST_URI="$path" SCRIPT_FILENAME="$T/app.lk" HTTP_HOST="127.0.0.1:$FP")
        [ -n "$ck" ] && pa+=(HTTP_COOKIE="$ck"); [ -n "$xt" ] && pa+=(HTTP_X_CSRF_TOKEN="$xt")
        [ -n "$ct" ] && pa+=(CONTENT_TYPE="$ct"); [ -n "$org" ] && pa+=(HTTP_ORIGIN="$org")
        BODY="$bf" python3 "$T/fcgi.py" $FP "${pa[@]}" > "$T/raw" 2>/dev/null; split_raw ;;
    cgi)
        local ev=(REQUEST_METHOD="$m" REQUEST_URI="$path" PATH_INFO="$path" SCRIPT_FILENAME="$T/app.lk"
                  GATEWAY_INTERFACE=CGI/1.1 HTTP_HOST="cgi.example" LOOK_SESSION_DIR="$T/s" LOOK_SECRET=$S1)
        [ -n "$ck" ] && ev+=(HTTP_COOKIE="$ck"); [ -n "$xt" ] && ev+=(HTTP_X_CSRF_TOKEN="$xt")
        [ -n "$ct" ] && ev+=(CONTENT_TYPE="$ct"); [ -n "$org" ] && ev+=(HTTP_ORIGIN="$org")
        [ -n "$bf" ] && ev+=(CONTENT_LENGTH="$(wc -c < "$bf")")
        (cd "$T" && env -i PATH=/usr/bin:/bin "${ev[@]}" "$CGI" < "${bf:-/dev/null}" > "$T/raw" 2>>"$T/cgilog"); split_raw ;;
    esac
}
ck()    { grep -i "^set-cookie: $1=" "$T/h" | tail -1 | sed 's/^[^:]*: //; s/;.*//' | tr -d '\r'; }
ckline(){ grep -i "^set-cookie: $1=" "$T/h" | tail -1 | tr -d '\r'; }
tok()   { grep -o 'name="_csrf" value="[^"]*"' "$T/b" | sed 's/.*value="//; s/"$//'; }
form()  { printf '%s' "$1" > "$T/fb"; }   # form <urlencoded body>

start() {   # start [env...] — --mode http on $P
    { kill $pid; wait $pid; } 2>/dev/null
    ( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SESSION_DIR="$T/s" "$@" "$LKF" --mode http --port $P --workers 2 app.lk >"$T/log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "$BASE/info" && return 0
        kill -0 $pid 2>/dev/null || return 1; sleep 0.2; done; return 1
}
start LOOK_SECRET=$S1 || { echo "FAIL: did not start"; cat "$T/log"; exit 1; }
( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET=$S1 LOOK_SESSION_DIR="$T/s" "$LKF" --port $FP --workers 2 --script "$T/app.lk" >"$T/flog" 2>&1 ) & fpid=$!
for i in $(seq 1 50); do python3 "$T/fcgi.py" $FP REQUEST_METHOD=GET REQUEST_URI=/info SCRIPT_FILENAME="$T/app.lk" >/dev/null 2>&1 && break; sleep 0.2; done

for mode in http fcgi cgi; do
    echo "== $mode: every state-changing request is checked"
    is "$mode: GET is not checked"                 "$(req $mode GET /form "" "" "" "")" 200
    C=$(ck look_csrf); K=$(tok)
    [ -n "$C" ] && [ -n "$K" ] && ok "$mode: csrf::field() sets the cookie and gives a token" || bad "$mode: no cookie or token"
    form "_csrf=$K&name=a"
    is "$mode: form with the token"                "$(req $mode POST /submit "$C" "" application/x-www-form-urlencoded "$T/fb")" 200
    grep -qi '^x-mw: ran' "$T/h" && ok "$mode: before_route ran for the accepted request" || bad "$mode: before_route did not run"
    form "name=a"
    is "$mode: form without the token: 403"        "$(req $mode POST /submit "$C" "" application/x-www-form-urlencoded "$T/fb")" 403
    grep -qi '^x-mw:' "$T/h" && bad "$mode: before_route ran for a refused request" || ok "$mode: a refused request reaches neither before_route nor the route"
    form "_csrf=$K"
    is "$mode: token without its cookie: 403"      "$(req $mode POST /submit "" "" application/x-www-form-urlencoded "$T/fb")" 403
    mpart "$K"
    is "$mode: multipart form with the token"      "$(req $mode POST /upload "$C" "" "multipart/form-data; boundary=$B" "$T/mp")" 200
    is "$mode: … and the route gets the file"      "$(cat "$T/b")" file
    mpart ""
    is "$mode: multipart form without the token: 403" "$(req $mode POST /upload "$C" "" "multipart/form-data; boundary=$B" "$T/mp")" 403
    printf '{"a":1}' > "$T/jb"
    is "$mode: JSON with X-CSRF-Token"             "$(req $mode POST /api/json "$C" "$K" application/json "$T/jb")" 200
    is "$mode: … and the route reads the JSON"     "$(cat "$T/b")" 'json:{"a":1}'
    is "$mode: JSON without X-CSRF-Token: 403"     "$(req $mode POST /api/json "$C" "" application/json "$T/jb")" 403
    for mth in PUT PATCH DELETE; do
        is "$mode: $mth without a token: 403"      "$(req $mode $mth /submit "$C" "" "" "")" 403
        is "$mode: $mth with X-CSRF-Token"         "$(req $mode $mth /submit "$C" "$K" "" "")" 200
    done
    form "_csrf=$K"
    is "$mode: Origin of another site: 403"        "$(req $mode POST /submit "$C" "" application/x-www-form-urlencoded "$T/fb" http://evil.example)" 403
    form "x=1"
    is "$mode: csrf::exempt(\"/hook\"): no token needed"           "$(req $mode POST /hook "" "" application/x-www-form-urlencoded "$T/fb")" 200
    is "$mode: csrf::exempt(\"/token-api/*\"): prefix"             "$(req $mode POST /token-api/42 "" "" application/x-www-form-urlencoded "$T/fb")" 200
    is "$mode: an exempt route that uses session:: is an error"    "$(req $mode POST /hook-session "" "" application/x-www-form-urlencoded "$T/fb")" 500
done
grep -q 'POST /submit refused: no _csrf field or X-CSRF-Token header. Every application that uses cookies is protected' "$T/log" \
    && ok "the log says why and what to do" || bad "no explanation in the log"
grep -q 'session::start() in a request exempted from the CSRF check by csrf::exempt("/hook-session")' "$T/log" \
    && ok "the log names the session call in an exempt route" || bad "no explanation for session:: in an exempt route"
is "csrf::exempt inside a route is an error" "$(req http GET /late-exempt "" "" "" "")" 500

echo "== the token"
req http GET /form "" "" "" "" >/dev/null; C=$(ck look_csrf); K=$(tok); L=$(ckline look_csrf)
[[ "$L" == *"Path=/"* && "$L" == *HttpOnly* && "$L" == *"SameSite=Lax"* && "$L" != *Secure* ]] && ok "cookie: Path=/, HttpOnly, SameSite=Lax, no Secure over http" || bad "cookie attributes: $L"
req http GET /tok "$C" "" "" "" >/dev/null; K2=$(cat "$T/b")
[ -z "$(ck look_csrf)" ] && ok "a second token reuses the cookie" || bad "a second token set a new cookie"
[ "$K2" != "$K" ] && ok "every token is different (fresh nonce)" || bad "the same token twice"
is "the second token is accepted too"         "$(req http POST /submit "$C" "$K2" "" "")" 200
req http GET /form "" "" "" "" >/dev/null; K3=$(tok)
is "another visitor's token: 403"             "$(req http POST /submit "$C" "$K3" "" "")" 403
bad_sig="${C%?}$( [ "${C: -1}" = 0 ] && echo 1 || echo 0 )"
is "a cookie with a changed signature: 403"   "$(req http POST /submit "$bad_sig" "$K" "" "")" 403
is "Origin of this site: accepted"            "$(req http POST /submit "$C" "$K" "" "" "http://127.0.0.1:$P")" 200
is "Origin: null: 403"                        "$(req http POST /submit "$C" "$K" "" "" null)" 403

echo "== request::scheme / request::host"
req http GET /info "" "" "" "" >/dev/null;   is "plain request" "$(cat "$T/b")" "http|127.0.0.1:$P"
curl -s -o "$T/b" -H "X-Forwarded-Proto: https" -H "X-Forwarded-Host: x.example" "$BASE/info"
is "X-Forwarded-* from an untrusted client is ignored" "$(cat "$T/b")" "http|127.0.0.1:$P"
curl -s -o "$T/b" -H "Host: Shop.Example.COM:8080" "$BASE/info"; is "host is lower case, port kept" "$(cat "$T/b")" "http|shop.example.com:8080"
curl -s -o "$T/b" -H "Host: evil.example/x" "$BASE/info";        is "a host with a path is not a host" "$(cat "$T/b")" "http|"
curl -s -o "$T/b" -H "Host: a@b" "$BASE/info";                    is "a host with @ is not a host" "$(cat "$T/b")" "http|"
is "FastCGI: HTTPS=on and HTTP_HOST" "$(python3 "$T/fcgi.py" $FP REQUEST_METHOD=GET REQUEST_URI=/info SCRIPT_FILENAME="$T/app.lk" HTTPS=on HTTP_HOST=Site.Example:8443 | tr -d '\r' | sed '1,/^$/d')" "https|site.example:8443"
is "FastCGI: X-Forwarded-Host from the client is ignored" "$(python3 "$T/fcgi.py" $FP REQUEST_METHOD=GET REQUEST_URI=/info SCRIPT_FILENAME="$T/app.lk" HTTP_HOST=site.example HTTP_X_FORWARDED_HOST=evil.example HTTP_X_FORWARDED_PROTO=https | tr -d '\r' | sed '1,/^$/d')" "http|site.example"
cg() { (cd "$T" && env -i PATH=/usr/bin:/bin REQUEST_METHOD=GET SCRIPT_FILENAME="$T/app.lk" PATH_INFO=/info REQUEST_URI=/info \
        GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" LOOK_SECRET=$S1 "$@" "$CGI" 2>/dev/null) | tr -d '\r' | sed '1,/^$/d'; }
is "CGI: HTTPS=on and HTTP_HOST"    "$(cg HTTPS=on HTTP_HOST=Site.Example)" "https|site.example"
printf 'route("GET", "/s", function() { session::start(); return response::text("s"); })\n' > "$T/sess.lk"
sc=$(cd "$T" && env -i PATH=/usr/bin:/bin REQUEST_METHOD=GET SCRIPT_FILENAME="$T/sess.lk" PATH_INFO=/s REQUEST_URI=/s \
     GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" LOOK_SECRET=$S1 HTTPS=on "$CGI" 2>/dev/null | tr -d '\r' | grep -i '^set-cookie: LOOK_SESSION=')
[[ "$sc" == *Secure* ]] && ok "CGI with HTTPS=on: the session cookie has Secure" || bad "CGI over HTTPS: session cookie without Secure: $sc"

echo "== a new token base at login and logout"
req http GET /login "$C" "" "" "" >/dev/null; CL=$(ck look_csrf)
[ -n "$CL" ] && [ "$CL" != "$C" ] && ok "session::regenerate gives a new token base" || bad "no new csrf cookie at login"
is "a token from before login: 403 after login" "$(req http POST /submit "$CL" "$K" "" "")" 403
req http GET /tok "$CL" "" "" "" >/dev/null; KL=$(cat "$T/b")
is "a token shown after login works"            "$(req http POST /submit "$CL" "$KL" "" "")" 200
req http GET /logout "$CL" "" "" "" >/dev/null; CO=$(ck look_csrf)
[ -n "$CO" ] && [ "$CO" != "$CL" ] && ok "session::destroy gives a new token base" || bad "no new csrf cookie at logout"

echo "== changing LOOK_SECRET"
start LOOK_SECRET=$S2 LOOK_SECRET_PREVIOUS=$S1 || { echo "FAIL: did not start"; exit 1; }
is "with LOOK_SECRET_PREVIOUS: an open form still works" "$(req http POST /submit "$C" "$K" "" "")" 200
req http GET /tok "$C" "" "" "" >/dev/null; CN=$(ck look_csrf); KN=$(cat "$T/b")
[ -n "$CN" ] && [ "$CN" != "$C" ] && [ "${CN%%.*}" = "${C%%.*}" ] && ok "the cookie is signed again with the new key" || bad "not re-signed: '$CN'"
start LOOK_SECRET=$S2 || { echo "FAIL: did not start"; exit 1; }
is "without LOOK_SECRET_PREVIOUS: the old cookie: 403" "$(req http POST /submit "$C" "$K" "" "")" 403
is "the re-signed cookie works"                         "$(req http POST /submit "$CN" "$KN" "" "")" 200

echo "== behind a trusted proxy"
start LOOK_SECRET=$S1 LOOK_TRUSTED_PROXY=127.0.0.1 || { echo "FAIL: did not start"; exit 1; }
PX=(-H "X-Forwarded-Proto: https" -H "X-Forwarded-Host: Shop.Example.com")
curl -s -o "$T/b" "${PX[@]}" "$BASE/info";   is "scheme and host from the trusted proxy" "$(cat "$T/b")" "https|shop.example.com"
curl -s -D "$T/h" -o "$T/b" "${PX[@]}" "$BASE/form"; CH=$(ck __Host-look_csrf); KH=$(tok); LH=$(ckline __Host-look_csrf)
[ -n "$CH" ] && [[ "$LH" == *Secure* && "$LH" != *Domain* ]] && ok "over https: __Host-look_csrf, Secure, no Domain" || bad "https cookie: $LH"
is "Origin = the forwarded host: accepted" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: $CH" -H "X-CSRF-Token: $KH" "${PX[@]}" -H "Origin: https://shop.example.com" "$BASE/submit")" 200
is "Origin of another site: 403"           "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Cookie: $CH" -H "X-CSRF-Token: $KH" "${PX[@]}" -H "Origin: https://evil.example" "$BASE/submit")" 403

echo "== which programs are checked"
# (Not inside $(…): the server started here must stay this shell's $pid.)
runs() {   # runs <program> [env...] → starts the program, POSTs / without a token; status (or "nostart") in $T/rs
    local prog="$1"; shift
    printf '%s\n' "$prog" > "$T/p.lk"; { kill $pid; wait $pid; } 2>/dev/null
    ( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SESSION_DIR="$T/s" "$@" "$LKF" --mode http --port $P --workers 2 p.lk >"$T/plog" 2>&1 ) & pid=$!
    for i in $(seq 1 30); do curl -s -o /dev/null --max-time 1 "$BASE/" && break
        kill -0 $pid 2>/dev/null || { echo nostart > "$T/rs"; return; }; sleep 0.2; done
    curl -s -o /dev/null -w '%{http_code}' -X POST -d x=1 "$BASE/" > "$T/rs"
}
runs 'route("GET", "/", fn() => response::text("g"))
route("POST", "/", fn() => response::text(request::header("Authorization", "none")))'
is "no cookies (token API): not checked, no LOOK_SECRET needed" "$(cat "$T/rs")" 200
runs 'route("GET", "/", fn() => response::text("g"))
route("POST", "/", fn() => response::text(cookie::get("jwt", "")))' LOOK_SECRET=$S1
is "only cookie::get (cookie set by the browser's JavaScript): checked" "$(cat "$T/rs")" 403
runs 'function remember($v) { cookie::set("theme", $v) }
route("GET", "/", fn() => response::text("g"))
route("POST", "/", function() { remember("dark"); return response::text("p"); })' LOOK_SECRET=$S1
is "only cookie::set in a helper function: checked" "$(cat "$T/rs")" 403
runs 'route("GET", "/", function() { session::start(); return response::text("g"); })
route("POST", "/", fn() => response::text("p"))'
is "a program with cookies and no LOOK_SECRET does not start" "$(cat "$T/rs")" nostart
grep -q "LOOK_SECRET is not set or shorter than 32 characters. This application uses cookies" "$T/plog" && grep -q "docker run -e LOOK_SECRET=" "$T/plog" \
    && ok "and says why, also for Docker" || bad "no reason given: $(tail -2 "$T/plog")"
runs 'route("GET", "/", function() { session::start(); return response::text("g"); })' LOOK_SECRET=short
is "a LOOK_SECRET shorter than 32 characters: does not start" "$(cat "$T/rs")" nostart

# Paket modülü (~/.look/modules/<ad>/<ad>.lk, `use <ad>`): çerezi uygulama değil paket kullanıyor —
# hazır bir kimlik doğrulama paketi gibi. Uygulamanın kendi kodunda çerez yok; koruma yine açık olmalı.
mkdir -p "$T/home/.look/modules/authpkg"
printf 'function current_user() { return cookie::get("jwt", "") }\n' > "$T/home/.look/modules/authpkg/authpkg.lk"
runs 'use authpkg
route("GET", "/", fn() => response::text("g"))
route("POST", "/", fn() => response::text("user=" . current_user()))' LOOK_SECRET=$S1 HOME="$T/home"
is "cookies only inside a package module (use authpkg): checked" "$(cat "$T/rs")" 403
runs 'use authpkg
route("GET", "/", fn() => response::text("g"))
route("POST", "/", fn() => response::text("p"))' LOOK_SECRET=$S1 HOME="$T/home"
is "a package that uses cookies, even if the app never calls it: checked" "$(cat "$T/rs")" 403
# Sıcak yeniden yükleme: derleyici bir paketi süreç başına bir kez derler (çift tanım olmasın). Yeniden
# yüklemede paket yeniden derlenmezse çerez kullanımı görünmez ve koruma sessizce kapanırdı.
printf 'use authpkg\nroute("GET", "/", fn() => response::text("g2"))\nroute("POST", "/", fn() => response::text("user=" . current_user()))\n' > "$T/p.lk"
touch -d '+2 seconds' "$T/p.lk"
for i in $(seq 1 30); do [ "$(curl -s --max-time 1 "$BASE/")" = g2 ] && break; sleep 0.3; done
is "after a hot reload the new program is served" "$(curl -s --max-time 2 "$BASE/")" g2
is "… and a package that uses cookies is still checked" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d x=1 "$BASE/")" 403

[ $fail = 0 ] && echo "PASS: CSRF protection is on by default (--mode http, FastCGI, CGI)" || echo "FAIL: CSRF protection"
exit $fail
