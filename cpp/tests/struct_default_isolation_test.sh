#!/usr/bin/env bash
# Struct default isolation. A default array/map was evaluated once at the declaration and
# shared by every instance. On the web that shared value outlives the request: what one
# request wrote into its own instance showed up in the NEXT user's freshly built instance.
# Two requests, both engines: the first mutates its instance (top level and nested), the
# second builds a new one and must see the declared defaults.
# Usage: struct_default_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7711}"
cat > "$TMP/app.lk" <<'LK'
use json
struct Cart { owner any = "nobody", items array = ["empty"], meta map = ["tags" => ["t0"]] }
function set_cart($u) {
    $c = Cart{owner: $u}
    $c.items[0] = $u
    $c.meta["tags"][0] = "secret-of-" . $u
    return $c
}
route("GET", "/set", fn() => response::json(set_cart(request::get("u"))))
route("GET", "/new", fn() => response::json(Cart{}))
LK
WANT='{"owner":"nobody","items":["empty"],"meta":{"tags":["t0"]}}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/new" && break; sleep 0.1; done
    first="$(curl -s -m 3 "localhost:$PORT/new")"
    a="$(curl -s -m 3 "localhost:$PORT/set?u=ali_card_1234")"
    b="$(curl -s -m 3 "localhost:$PORT/new")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    case "$a" in *'"items":["ali_card_1234"]'*'secret-of-ali_card_1234'*) ;; *) echo "  FAIL [$mode] the first request did not mutate its own instance: $a"; fail=1 ;; esac
    if [ "$first" = "$WANT" ] && [ "$b" = "$WANT" ]; then echo "  OK   [$mode] the next request sees the declared defaults"
    else echo "  FAIL [$mode] another request's data leaked into a new instance: $b"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: struct defaults are per instance" || { echo "FAIL: struct default isolation"; exit 1; }
