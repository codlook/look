#!/usr/bin/env bash
# Request isolation. A web script's top-level variables are created once, at setup, and every
# request uses them. A request that WRITES one of them must not change what the next request
# reads.
#
# LOOK 2 closes most of this by rule rather than by copying:
#   * values: assigning or passing an array gives a copy, so a write through an alias cannot
#     reach the original (tests/value_semantics_test.lk);
#   * captured variables: a closure captures the VALUE and may not change it. A route or a
#     middleware that writes to something it captured does not load at all (checked below),
#     so "the variable every request shares through a closure" no longer exists.
# What is left, and what this test drives, are the top-level variables themselves: a function
# can still write to a global, and the server gives each request its own.
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
$P = ["p0"]
$Q = ["k" => ["q0"]]
$S = ["s0", "s1"]
function w_global($u) { $G[0] = $u; $N["k"][0] = $u; push($G, $u); return count($G) }
// push and pop are the FIRST write these three ever see (no assignment before them)
function w_push($u) { push($P, $u); push($Q["k"], $u); pop($S); return count($P) + count($Q["k"]) + count($S) }
route("GET", "/w", function() use ($U) {
    $mine = $U
    $mine[0] = request::get("u")
    return response::json([w_global(request::get("u")), $G[0], $N["k"][0], $mine[0], $U[0], w_push(request::get("u"))])
})
route("GET", "/r", fn() use ($U) => response::json(["const" => LIST[0], "global" => $G[0], "nested" => $N["k"][0],
                                                    "use" => $U[0], "count" => count($G),
                                                    "pushed" => count($P), "nested_pushed" => count($Q["k"]), "popped" => count($S)]))
LK
WANT='{"const":"c0","global":"g0","nested":"n0","use":"u0","count":1,"pushed":1,"nested_pushed":1,"popped":2}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    before="$(curl -s -m 3 "localhost:$PORT/r")"
    w="$(curl -s -m 3 "localhost:$PORT/w?u=LEAK")"
    after="$(curl -s -m 3 "localhost:$PORT/r")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    # The writing request must see its own writes; the captured value stays what it was.
    [ "$w" = '[2,"LEAK","LEAK","LEAK","u0",5]' ] || { echo "  FAIL [$mode] the writing request did not see its own writes: $w $(grep -m1 ERROR "$TMP/log.txt" | cut -c1-160)"; fail=1; }
    if [ "$before" = "$WANT" ] && [ "$after" = "$WANT" ]; then echo "  OK   [$mode] the next request reads the declared values"
    else echo "  FAIL [$mode] before=$before"; echo "       [$mode] after =$after"; fail=1; fi
done

# A handler that writes to what it captured must not load: nothing is served.
cat > "$TMP/bad1.lk" <<'LK'
$U = ["u0"]
route("GET", "/x", function() use ($U) { $U[0] = request::get("u") })
route("GET", "/ok", fn() => "served")
LK
cat > "$TMP/bad2.lk" <<'LK'
$U = ["u0"]
before_route(function() use ($U) { push($U, 1) })
route("GET", "/ok", fn() => "served")
LK
cat > "$TMP/bad3.lk" <<'LK'
function mk() { $n = [0]
  return function() { $n[0] = $n[0] + 1
    return $n[0] } }
route("GET", "/ok", fn() => "served")
LK
for app in bad1 bad2 bad3; do
    for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
        ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 $app.lk > "$TMP/bad.txt" 2>&1 ) & pid=$!
        sleep 0.8; got="$(curl -s -m 2 "localhost:$PORT/ok")"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        if [ "$got" != "served" ] && grep -q "captures the value and cannot change it" "$TMP/bad.txt"; then echo "  OK   [$mode] refused at load: $app"
        else echo "  FAIL [$mode] $app: a handler writing to a captured variable was loaded (got=[$got]): $(head -2 "$TMP/bad.txt" | tr '\n' ' ' | cut -c1-200)"; fail=1; fi
    done
done
[ $fail = 0 ] && echo "PASS: requests are isolated from each other" || { echo "FAIL: request isolation"; exit 1; }
