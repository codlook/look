#!/usr/bin/env bash
# A session id the server did not issue is never used (session fixation), and cookie::set cannot be
# made to write extra cookie attributes.
#
# session::start() checked only the shape of the LOOK_SESSION cookie (32 hex digits). An attacker
# who could plant that cookie in the victim's browser (a sibling subdomain, plain HTTP) chose the
# id; after the victim logged in, the attacker's browser — same id — was logged in as the victim.
# session::set() without start wrote to any such id too. Now only an id this server issued (and
# that has not expired) is used; any other gets a fresh id. A session with no data yet keeps its
# id across requests.
# cookie::set(name, value, path) put the path into Set-Cookie as given (only CR/LF removed), so a
# path from user input could add "; Domain=evil.example; Max-Age=…". A ';' or control character in
# the path, or an unknown SameSite value, is now an error.
# Usage: session_fixation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7747}"
cat > "$TMP/app.lk" <<'LK'
route("GET", "/login", function() { session::start(); session::set("user", "victim"); return response::text("in") })
route("GET", "/set-only", function() { session::set("user", "victim2"); return response::text("set") })
route("GET", "/whoami", fn() => response::text(session::get("user") ?? "none"))
route("GET", "/start", fn() => response::text(session::start()))
route("GET", "/regen", function() { session::start(); session::regenerate(); session::set("k", "v"); return response::text(session::start()) })
route("GET", "/pref", function() { cookie::set("theme", "dark", request::get("p", "/")); return response::text("ok") })
route("GET", "/ss", function() { cookie::set("t", "x", ["samesite" => request::get("s", "Lax")]); return response::text("ok") })
LK
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
( cd "$TMP" && exec env LOOK_SECRET=fix-test-0123456789abcdef0123456789abcdef LOOK_SESSION_DIR="$TMP/s" \
    "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/start" && break; sleep 0.1; done
U="http://localhost:$PORT"
PLANT="LOOK_SESSION=deadbeefdeadbeefdeadbeefdeadbeef"
sid_of() { grep -io 'set-cookie: LOOK_SESSION=[0-9a-f]*' | head -1 | cut -d= -f2; }

new="$(curl -s -D - -o /dev/null -H "Cookie: $PLANT" "$U/login" | sid_of)"
[ -n "$new" ] && [ "$new" != deadbeefdeadbeefdeadbeefdeadbeef ] && ok "a planted id gets a fresh one at session::start" || bad "planted id kept by start (new='$new')"
[ "$(curl -s -H "Cookie: $PLANT" "$U/whoami")" = none ] && ok "the planted id does not see the victim's session" || bad "the planted id sees the victim's session"
[ "$(curl -s -H "Cookie: LOOK_SESSION=$new" "$U/whoami")" = victim ] && ok "the issued id does" || bad "the issued id lost the session"

new2="$(curl -s -D - -o /dev/null -H "Cookie: LOOK_SESSION=cafebabecafebabecafebabecafebabe" "$U/set-only" | sid_of)"
[ -n "$new2" ] && [ "$(curl -s -H "Cookie: LOOK_SESSION=cafebabecafebabecafebabecafebabe" "$U/whoami")" = none ] \
    && ok "session::set without start does not write to a planted id" || bad "session::set wrote to a planted id (new='$new2')"

s1="$(curl -s -c "$TMP/j" "$U/start")"; s2="$(curl -s -b "$TMP/j" -D "$TMP/h" "$U/start")"
[ -n "$s1" ] && [ "$s1" = "$s2" ] && ! grep -qi 'set-cookie: LOOK_SESSION' "$TMP/h" \
    && ok "a session with no data keeps its id across requests" || bad "empty session changed id ($s1 → $s2)"
r="$(curl -s -D "$TMP/h2" "$U/regen")"
[ "$(grep -ci 'set-cookie: LOOK_SESSION=[0-9a-f]' "$TMP/h2")" -le 2 ] && [ "$(sid_of < "$TMP/h2" | tail -1)" != "" ] \
    && [ "$(grep -io 'set-cookie: LOOK_SESSION=[0-9a-f]*' "$TMP/h2" | tail -1 | cut -d= -f2)" = "$r" ] \
    && ok "after regenerate the session keeps the regenerated id" || bad "regenerate: id '$r' is not the last cookie sent"

code="$(curl -s -o /dev/null -w '%{http_code}' -D "$TMP/h3" "$U/pref?p=%2F%3B%20Domain%3Devil.example")"
[ "$code" = 500 ] && ! grep -qi 'domain=evil' "$TMP/h3" && ok "a ';' in the cookie path is an error, no Domain injected" \
    || bad "cookie path injection: http $code, $(grep -i set-cookie "$TMP/h3" | tr -d '\r')"
grep -q "cookie::set() path must not contain ';'" "$TMP/log.txt" && ok "the log names the cookie path problem" || bad "no log line for the cookie path"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$U/ss?s=Lax%3B%20Domain%3Devil")" = 500 ] && ok "an unknown SameSite value is an error" || bad "SameSite injection accepted"
curl -s -D - -o /dev/null "$U/ss?s=strict" | grep -qi 'samesite=Strict' && ok "SameSite strict is written as Strict" || bad "SameSite strict not written"

kill -0 "$pid" 2>/dev/null && ok "the server is still running" || bad "the server died"
# A short LOOK_SECRET_PREVIOUS still verified CSRF cookies: a guessable previous key let anyone sign
# their own. Now the application does not start with one, and says why.
( cd "$TMP" && exec env LOOK_SECRET=fix-test-0123456789abcdef0123456789abcdef LOOK_SECRET_PREVIOUS=x \
    LOOK_SESSION_DIR="$TMP/s2" timeout 10 "$FCGI" --mode http --port $((PORT + 1)) --workers 1 app.lk > "$TMP/log2.txt" 2>&1 ); rc=$?
[ $rc != 0 ] && [ $rc != 124 ] && grep -q "LOOK_SECRET_PREVIOUS is shorter than 32 characters" "$TMP/log2.txt" \
    && ok "a short LOOK_SECRET_PREVIOUS stops the start with the reason" || bad "short LOOK_SECRET_PREVIOUS: exit $rc, $(tail -1 "$TMP/log2.txt")"
[ $fail = 0 ] && echo "PASS: only issued session ids are used; cookie attributes cannot be injected" \
             || { echo "FAIL: session fixation / cookie attributes"; exit 1; }
