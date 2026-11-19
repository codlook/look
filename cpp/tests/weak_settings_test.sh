#!/usr/bin/env bash
# Every setting that weakens security writes a "[SECURITY]" WARN line when the server starts (look/weak_settings.h);
# with default settings none is written. OLD: these settings were silent — a setting turned on for testing and forgotten in production
# left no trace.
# Usage: weak_settings_test.sh <lk-fcgi> [port]
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7651}"
TMP="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
echo 'route("GET", "/ok", fn() => response::text("ok"))' > "$TMP/app.lk"

# <mode http|fcgi> <env...> → $log = the server's startup log
start_log() {
    local mode=$1; shift
    if [ "$mode" = http ]; then
        ( cd "$TMP" && exec env "$@" LOOK_SECRET=weak-test-secret-0123456789abcdef \
            "$FCGI" --mode http --port "$PORT" app.lk > "$TMP/log" 2>&1 ) & pid=$!
    else
        ( cd "$TMP" && exec env "$@" LOOK_SECRET=weak-test-secret-0123456789abcdef \
            "$FCGI" --port "$PORT" app.lk > "$TMP/log" 2>&1 ) & pid=$!
    fi
    for i in $(seq 1 50); do grep -qE "Listening|TCP mode" "$TMP/log" 2>/dev/null && break; sleep 0.1; done
    sleep 0.3
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    log="$(grep '\[SECURITY\]' "$TMP/log")"
    PORT=$((PORT + 1))
}
has() { [[ "$log" == *"$1"* ]] && echo ok; }

start_log http
check "default settings: no security warning" "$( [ -z "$log" ] && echo ok)" "$log"
start_log fcgi
check "default FastCGI (127.0.0.1): no security warning" "$( [ -z "$log" ] && echo ok)" "$log"
start_log http LOOK_TRUSTED_PROXY=127.0.0.1,10.0.0.0/8 LOOK_WS_ORIGINS=https://a.example LOOK_SESSION_SECURE=1
check "safe values of the same settings: no warning" "$( [ -z "$log" ] && echo ok)" "$log"

start_log http LOOK_ALLOW_SSRF=1
check "LOOK_ALLOW_SSRF=1" "$(has 'LOOK_ALLOW_SSRF=1: http:: requests may reach')" "$log"
start_log http LOOK_SESSION_SECURE=0
check "LOOK_SESSION_SECURE=0" "$(has 'LOOK_SESSION_SECURE=0: the session and CSRF cookies')" "$log"
start_log http "LOOK_WS_ORIGINS=https://a.example, *"
check "LOOK_WS_ORIGINS with *" "$(has 'LOOK_WS_ORIGINS contains *')" "$log"
start_log http LOOK_TRUSTED_PROXY=10.0.0.1,0.0.0.0/0
check "LOOK_TRUSTED_PROXY=0.0.0.0/0" "$(has 'LOOK_TRUSTED_PROXY trusts 0.0.0.0/0')" "$log"
start_log http APP_ENV=development
check "APP_ENV=development" "$(has 'APP_ENV=development: error responses show error details')" "$log"
start_log http APP_DEBUG=true
check "APP_DEBUG=true" "$(has 'APP_DEBUG=true: error responses show error details')" "$log"
start_log fcgi LOOK_FCGI_BIND=0.0.0.0
check "FastCGI LOOK_FCGI_BIND=0.0.0.0" "$(has 'LOOK_FCGI_BIND=0.0.0.0: the FastCGI port accepts connections from other machines')" "$log"
start_log http LOOK_ALLOW_SSRF=1 APP_DEBUG=true
check "two settings: two lines" "$( [ "$(printf '%s\n' "$log" | grep -c SECURITY)" = 2 ] && echo ok)" "$log"

[ $fail = 0 ] && echo "PASS: settings that weaken security are named at start" || echo "FAIL: weak settings warnings"
exit $fail
