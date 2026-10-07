#!/usr/bin/env bash
# Path parameters reach a route handler by POSITION in LOOK 1. A handler whose parameter
# names do not match the names in its path gets the wrong value without any error:
#     route("GET", "/c/{id}/{slug}", function($slug) { ... })      $slug holds the id
# The behaviour is unchanged (changing it would break applications), but such a handler is
# now reported at startup, once per parameter. A handler that matches, or that takes no
# parameters and uses request::param, produces no warning.
# Usage: route_param_warning_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7727}"
cat > "$TMP/app.lk" <<'LK'
use json
route("GET", "/ok/{id}/{slug}", function($id, $slug) { return response::json([$id, $slug]) })
route("GET", "/one/{id}", function($id) { return response::json([$id]) })
route("GET", "/none/{id}", function() { return response::json([request::param("id")]) })
route("GET", "/wrong/{id}/{slug}", function($slug) { return response::json([$slug]) })
route("GET", "/extra/{id}", function($id, $more) { return response::json([$id, $more]) })
route("SSE", "/s/{room}", function($conn, $room) { sse::close($conn) })
LK
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/one/1" && break; sleep 0.1; done
    wrong="$(curl -s -m 3 "localhost:$PORT/wrong/7/s1")"; ok="$(curl -s -m 3 "localhost:$PORT/ok/7/s1")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    n="$(grep -c 'handler parameter' "$TMP/log.txt")"
    w1="$(grep -c "route GET /wrong/{id}/{slug}: handler parameter \$slug receives the path parameter 'id'" "$TMP/log.txt")"
    w2="$(grep -c 'route GET /extra/{id}: handler parameter $more is always null' "$TMP/log.txt")"
    if [ "$n" = "2" ] && [ "$w1" = "1" ] && [ "$w2" = "1" ]; then echo "  OK   [$mode] two mismatched handlers are reported at startup, the four correct ones are not"
    else echo "  FAIL [$mode] warnings=$n wrong=$w1 extra=$w2: $(grep 'handler parameter' "$TMP/log.txt" | cut -c1-200 | head -3)"; fail=1; fi
    if [ "$wrong" = '["7"]' ] && [ "$ok" = '["7","s1"]' ]; then echo "  OK   [$mode] behaviour is unchanged (parameters still arrive by position)"
    else echo "  FAIL [$mode] wrong=$wrong ok=$ok"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: handlers whose parameter names do not match their path are reported" || { echo "FAIL: route parameter warning"; exit 1; }
