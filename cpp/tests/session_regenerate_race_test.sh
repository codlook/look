#!/usr/bin/env bash
# session::regenerate under concurrency (scs model): requests that started with the same session
# before one of them regenerated it each keep the data under their new id. Only one of eight used
# to: the first regenerate removed the old record and the others found nothing (the visitor looked
# logged out). A logout (session::destroy) during a regenerate is not undone: the copy a request
# took at its start is used only when another regenerate moved the session, never after a logout.
# The old id is dead afterwards either way (session fixation).
# Usage: session_regenerate_race_test.sh <lk-fcgi> [redis-url] [port]
set -u
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
URL="${2:-}"; PORT="${3:-7931}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
[ -n "$URL" ] && export LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="$URL"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("GET", "/login", function() { session::start(); session::set("user", "alice"); return response::text("ok") })
route("GET", "/who", function() { session::start(); return response::text("user=" . (session::get("user") ?? "none")) })
route("GET", "/logout", function() { session::start(); session::destroy(); return response::text("bye") })
# ~1 s between start and regenerate, so concurrent requests all start before the first regenerates
route("GET", "/slowregen", function() { session::start(); $n = 0; for ($i = 0; $i < 30000000; $i++) { $n = $n + 1 } session::regenerate(); return response::text("ok") })
LK
mkdir -p "$T/sess"; chmod 700 "$T/sess"
( cd "$T" && LOOK_SESSION_DIR="$T/sess" exec "$FCGI" --mode http --port "$PORT" --workers 16 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/who" && break; sleep 0.1; done
B="http://localhost:$PORT"; fail=0; drv=${URL:+redis}; drv=${drv:-file}
for round in 1 2; do
    curl -s -c "$T/j0" "$B/login" > /dev/null
    old="$(grep LOOK_SESSION "$T/j0" | awk '{print $7}')"
    cps=""; for i in $(seq 1 8); do (cp "$T/j0" "$T/j$i"; curl -s -m 30 -b "$T/j$i" -c "$T/j$i" "$B/slowregen" > /dev/null) & cps="$cps $!"; done
    wait $cps
    kept=0; for i in $(seq 1 8); do [ "$(curl -s -b "$T/j$i" "$B/who")" = "user=alice" ] && kept=$((kept+1)); done
    [ $kept = 8 ] && echo "  OK   [$drv] 8 concurrent regenerates: every new id kept the data" \
                  || { echo "  FAIL [$drv] 8 concurrent regenerates: $kept/8 new ids kept the data"; fail=1; }
    w="$(curl -s -H "Cookie: LOOK_SESSION=$old" "$B/who")"
    [ "$w" = "user=none" ] && echo "  OK   [$drv] the old id is dead" || { echo "  FAIL [$drv] the old id still works: $w"; fail=1; }
    curl -s -c "$T/k0" "$B/login" > /dev/null; cp "$T/k0" "$T/kb"
    ( curl -s -m 30 -b "$T/kb" -c "$T/kb" "$B/slowregen" > /dev/null ) & b=$!
    sleep 0.3; curl -s -b "$T/k0" "$B/logout" > /dev/null; wait $b
    w="$(curl -s -b "$T/kb" "$B/who")"
    [ "$w" = "user=none" ] && echo "  OK   [$drv] a logout during a regenerate is not undone" \
                           || { echo "  FAIL [$drv] the logout was undone by a regenerate: $w"; fail=1; }
done
[ $fail = 0 ] && echo "PASS [$drv]: concurrent regenerate keeps the session, a logout stays a logout" || { echo "FAIL [$drv]: session regenerate race"; exit 1; }
