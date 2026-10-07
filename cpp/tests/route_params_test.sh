#!/usr/bin/env bash
# LOOK 2: a route handler takes path parameters by NAME.
#
#   route("GET", "/c/{id}/{slug}", function($slug) { ... })     $slug is the slug
#
# They used to be passed by position: the handler above silently received the id, and a
# parameter the route does not have was silently null. Now each handler parameter takes the
# path parameter of the same name, in any order and any subset (the rest is read with
# request::param), and a name the route does not have is an error when the route is
# registered — the application does not start. In a WS or SSE route the first parameter is
# the connection.
# Usage: route_params_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7726}"
cat > "$TMP/app.lk" <<'LK'
use json
route("GET", "/one/{id}/{slug}", function($slug) { return response::json(["slug" => $slug, "id" => request::param("id")]) })
route("GET", "/swap/{id}/{slug}", function($slug, $id) { return response::json(["slug" => $slug, "id" => $id]) })
route("GET", "/both/{id}/{slug}", function($id, $slug) { return response::json(["slug" => $slug, "id" => $id]) })
route("GET", "/none/{id}", function() { return response::json(["id" => request::param("id")]) })
route("GET", "/dec/{name}", function($name) { return response::json(["name" => $name]) })
LK
fail=0
WANT='{"slug":"s1","id":"7"}{"slug":"s1","id":"7"}{"slug":"s1","id":"7"}{"id":"7"}{"name":"a b"}'
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/none/1" && break; sleep 0.1; done
    got=""; for u in one/7/s1 swap/7/s1 both/7/s1 none/7 dec/a%20b; do got="$got$(curl -s -m 3 "localhost:$PORT/$u")"; done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = "$WANT" ]; then echo "  OK   [$mode] parameters arrive by name: one of two, swapped, both, none, URL-decoded"
    else echo "  FAIL [$mode] $got"; fail=1; fi
done

# A handler parameter that is not a path parameter of its route: the application does not start.
refused() {  # label, route line
    printf 'route("GET", "/ok", fn() => "served")\n%s\n' "$2" > "$TMP/bad.lk"
    for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
        ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 bad.lk > "$TMP/bad.txt" 2>&1 ) & pid=$!
        sleep 0.8; got="$(curl -s -m 2 "localhost:$PORT/ok")"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        if [ "$got" != "served" ] && grep -q "$3" "$TMP/bad.txt"; then echo "  OK   [$mode] refused at startup: $1"
        else echo "  FAIL [$mode] $1: started (got=[$got]) or wrong message: $(head -2 "$TMP/bad.txt" | tr '\n' ' ' | cut -c1-180)"; fail=1; fi
    done
}
refused "a name the route does not have" 'route("GET", "/x/{id}", function($slug) { return "x" })' \
        'route GET /x/{id}: handler parameter $slug is not a path parameter of this route (it has: id)'
refused "one right, one extra"           'route("GET", "/x/{id}", function($id, $extra) { return "x" })' \
        'handler parameter $extra is not a path parameter of this route (it has: id)'
refused "a parameter on a route without path parameters" 'route("GET", "/x", function($id) { return "x" })' \
        'handler parameter $id is not a path parameter of this route (it has none)'
[ $fail = 0 ] && echo "PASS: route handlers take path parameters by name" || { echo "FAIL: route parameters"; exit 1; }
