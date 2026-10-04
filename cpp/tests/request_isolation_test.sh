#!/usr/bin/env bash
# Request isolation. A web script's top-level variables and the variables its routes and
# closures capture are created once, at setup, and every request uses them. A request that
# WRITES one of those variables must not change what the next request reads.
#
# LOOK 2 note: values are isolated by the language itself (assigning or passing an array
# gives a copy, so writing through an alias cannot touch the original). What is tested here
# is the other half, which copy on write does NOT give: the VARIABLES. Each write below goes
# straight at a shared variable — a global, a variable captured with use, the variable a
# stored closure closed over — because those are every request's variables unless the server
# gives each request its own.
# One request writes through every path; the next request must read the declared values.
# Usage: request_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7714}"
cat > "$TMP/app.lk" <<'LK'
use json
const { LIST = ["c0"] }
$G = ["g0"]
$N = ["k" => ["n0"]]
$U = ["u0"]
$M = ["m0"]
function make_counter() { $n = [0]; return function() use ($n) { $n[0] = $n[0] + 1; return $n[0] } }
$counter = make_counter()
function w_global($u) { $G[0] = $u; $N["k"][0] = $u; return 1 }
before_route(function() use ($M) { if (request::get("u")) { $M[0] = request::get("u") } })
route("GET", "/w", function() use ($U) {
    $U[0] = request::get("u")
    return response::json([w_global(request::get("u")), $counter(), $counter(), $U[0]])
})
route("GET", "/r", fn() use ($U, $M) => response::json(["const" => LIST[0], "global" => $G[0], "nested" => $N["k"][0],
                                                        "use" => $U[0], "middleware" => $M[0], "counter" => $counter()]))
LK
WANT='{"const":"c0","global":"g0","nested":"n0","use":"u0","middleware":"m0","counter":1}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    before="$(curl -s -m 3 "localhost:$PORT/r")"
    w="$(curl -s -m 3 "localhost:$PORT/w?u=LEAK")"
    after="$(curl -s -m 3 "localhost:$PORT/r")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    # The writing request must see its own writes: the counter counts 1, 2 and $U holds the new value.
    [ "$w" = '[1,1,2,"LEAK"]' ] || { echo "  FAIL [$mode] the writing request did not see its own writes: $w $(grep -m1 ERROR "$TMP/log.txt" | cut -c1-160)"; fail=1; }
    if [ "$before" = "$WANT" ] && [ "$after" = "$WANT" ]; then echo "  OK   [$mode] the next request reads the declared values"
    else echo "  FAIL [$mode] before=$before"; echo "       [$mode] after =$after"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: requests are isolated from each other" || { echo "FAIL: request isolation"; exit 1; }
