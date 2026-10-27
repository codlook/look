#!/usr/bin/env bash
# Session and cookie security, the same checks in --mode http, FastCGI and CGI.
#
# These were tested in --mode http only (cookie_flags_test, session_secure), and CGI fell behind
# again and again: expired sessions were never removed, HTTPS was never read (the session cookie
# went out without Secure), and request headers never reached the application. Every check here
# runs in all three modes, so a mode that lacks a behaviour turns CI red:
#   cookie::set flags · the session cookie's Secure (real HTTPS only; a client's
#   X-Forwarded-Proto never counts; LOOK_SESSION_SECURE forces it; a warning when it is left
#   out) · session files 0600 in a 0700 directory · a client-chosen session id is not trusted ·
#   request headers reach the application.
# Usage: session_cookie_modes_test.sh <lk-fcgi> <lk-cgi>   (needs curl, python3)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
APPF="$(mktemp --suffix=.lk)"
cat > "$APPF" <<'EOF'
route("GET", "/", fn() => response::text("up"))
route("GET", "/default", function() { cookie::set("sid", "abc"); return response::text("ok"); })
route("GET", "/optout", function() { cookie::set("jsread", "xyz", ["httponly" => false]); return response::text("ok"); })
route("GET", "/secure", function() { cookie::set("s", "v", ["secure" => true, "samesite" => "Strict"]); return response::text("ok"); })
route("GET", "/expires", function() { cookie::set("e", "v", 3600, "/app"); return response::text("ok"); })
route("GET", "/nosamesite", function() { cookie::set("n", "v", ["samesite" => false]); return response::text("ok"); })
route("GET", "/set", function() { session::start(); session::set("u", "ali"); return response::text("ok"); })
route("GET", "/get", function() { session::start(); return response::text("u=" . session::get("u", "none")); })
route("GET", "/hdr", fn() => response::text(request::header("Authorization", "none") . "|" . request::header("X-Custom", "none")))
EOF
source "$(dirname "$0")/three_modes.sh" "$1" "$2" "$APPF"
trap 'tm_cleanup; rm -f "$APPF"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
has() { [[ "$1" == *"$2"* ]]; }

echo "== cookie::set flags"
tm_start || exit 1
for m in $TM_MODES; do
    tm_req $m GET /default >/dev/null;    L=$(tm_cookie_line sid)
    has "$L" HttpOnly && has "$L" "SameSite=Lax" && has "$L" "Path=/" && ! has "$L" Secure \
        && ok "$m: default: HttpOnly, SameSite=Lax, Path=/, no Secure" || bad "$m: default flags: $L"
    tm_req $m GET /optout >/dev/null;     L=$(tm_cookie_line jsread)
    ! has "$L" HttpOnly && has "$L" "SameSite=Lax" && ok "$m: httponly => false" || bad "$m: opt-out: $L"
    tm_req $m GET /secure >/dev/null;     L=$(tm_cookie_line s)
    has "$L" Secure && has "$L" "SameSite=Strict" && ok "$m: secure => true, samesite => Strict" || bad "$m: secure: $L"
    tm_req $m GET /expires >/dev/null;    L=$(tm_cookie_line e)
    has "$L" "Path=/app" && has "$L" "Expires=" && has "$L" HttpOnly && ok "$m: positional expires and path" || bad "$m: positional: $L"
    tm_req $m GET /nosamesite >/dev/null; L=$(tm_cookie_line n)
    ! has "$L" SameSite && has "$L" HttpOnly && ok "$m: samesite => false" || bad "$m: samesite opt-out: $L"
done

echo "== the session cookie over plain HTTP, and the session itself"
for m in $TM_MODES; do
    tm_req $m GET /set >/dev/null; L=$(tm_cookie_line LOOK_SESSION); C=$(tm_cookie LOOK_SESSION)
    [ -n "$C" ] && ! has "$L" Secure && ok "$m: plain HTTP: session cookie without Secure" || bad "$m: plain HTTP: $L"
    tm_req $m GET /get -c "$C" >/dev/null
    [ "$(cat "$TM/b")" = u=ali ] && ok "$m: the session round-trips" || bad "$m: round-trip: $(cat "$TM/b")"
    sid="${C#LOOK_SESSION=}"; f="$TM/s/look_sess_$sid"
    [ "$(stat -c %a "$f" 2>/dev/null)" = 600 ] && ok "$m: the session file is 0600" || bad "$m: session file mode: $(stat -c %a "$f" 2>&1)"
    tm_req $m GET /get -c "LOOK_SESSION=00000000000000000000000000000000" >/dev/null
    [ "$(cat "$TM/b")" = u=none ] && ok "$m: a client-chosen session id gives an empty session" || bad "$m: chosen id: $(cat "$TM/b")"
    tm_req $m GET /get -c "LOOK_SESSION=../../etc/passwd" >/dev/null
    N=$(tm_cookie LOOK_SESSION)
    [ "$(cat "$TM/b")" = u=none ] && [[ "$N" =~ ^LOOK_SESSION=[0-9a-f]{32}$ ]] && ok "$m: a malformed session id is replaced by a new one" || bad "$m: malformed id: $(cat "$TM/b") $N"
done
[ "$(stat -c %a "$TM/s")" = 700 ] && ok "the session directory is 0700" || bad "session directory mode: $(stat -c %a "$TM/s")"
tm_log | grep -q 'WITHOUT Secure' && ok "a session cookie without Secure is logged as a warning" || bad "no warning for a session cookie without Secure"

echo "== request headers reach the application"
for m in $TM_MODES; do
    tm_req $m GET /hdr -H "Authorization: Bearer t0k" -H "X-Custom: v1" >/dev/null
    [ "$(cat "$TM/b")" = "Bearer t0k|v1" ] && ok "$m: request::header()" || bad "$m: request::header(): $(cat "$TM/b")"
done

echo "== HTTPS: Secure, and only from the web server or a trusted proxy"
for m in $TM_MODES; do
    tm_req $m GET /set -H "X-Forwarded-Proto: https" >/dev/null; L=$(tm_cookie_line LOOK_SESSION)
    ! has "$L" Secure && ok "$m: a client's X-Forwarded-Proto does not count" || bad "$m: forged X-Forwarded-Proto gave Secure: $L"
done
tm_start LOOK_TRUSTED_PROXY=127.0.0.1 || exit 1
for m in $TM_MODES; do
    tm_req $m GET /set --https >/dev/null; L=$(tm_cookie_line LOOK_SESSION)
    has "$L" Secure && ok "$m: HTTPS: session cookie with Secure" || bad "$m: HTTPS without Secure: $L"
done
tm_start LOOK_SESSION_SECURE=1 || exit 1
for m in $TM_MODES; do
    tm_req $m GET /set >/dev/null; L=$(tm_cookie_line LOOK_SESSION)
    has "$L" Secure && ok "$m: LOOK_SESSION_SECURE=1 forces Secure" || bad "$m: =1 without Secure: $L"
done
tm_start LOOK_SESSION_SECURE=0 LOOK_TRUSTED_PROXY=127.0.0.1 || exit 1
for m in $TM_MODES; do
    tm_req $m GET /set --https >/dev/null; L=$(tm_cookie_line LOOK_SESSION)
    ! has "$L" Secure && ok "$m: LOOK_SESSION_SECURE=0 leaves Secure out" || bad "$m: =0 with Secure: $L"
done

[ $fail = 0 ] && echo "PASS: session and cookie security in --mode http, FastCGI and CGI" || echo "FAIL: session and cookie security"
exit $fail
