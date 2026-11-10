#!/usr/bin/env bash
# Request isolation. A web script's top-level variables are created once, at setup, and every
# request uses them. A request that WRITES one of them must not change what the next request
# reads.
#
# LOOK closes most of this by rule rather than by copying:
#   * values: assigning or passing an array gives a copy, so a write through an alias cannot
#     reach the original (tests/value_semantics_test.lk);
#   * captured variables: a closure captures the VALUE and may not change it. A route or a
#     middleware that writes to something it captured does not load at all (checked below),
#     so "the variable every request shares through a closure" no longer exists.
#   * top-level variables: a function, closure or route handler may read them but not change
#     them in place — that does not load either (checked below; every in-place write is in
#     tests/outer_write_test.sh).
# What this test drives is the data every request shares: a request takes the top-level values
# into its own variables (directly, or through a function that takes a parameter and returns the
# new value) and changes them through every write path — the shared setup data must not change.
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
// takes the shared values as parameters, changes them, returns the new values
function w_global($g, $n, $u) { $g[0] = $u; $n["k"][0] = $u; push($g, $u); return [$g, $n] }
// push and pop are the FIRST write these three see (no assignment before them)
function w_push($p, $q, $s, $u) { push($p, $u); push($q["k"], $u); pop($s); return [$p, $q, $s] }
function counted($pqs) { return count($pqs[0]) + count($pqs[1]["k"]) + count($pqs[2]) }
route("GET", "/w", function() use ($U) {
    $mine = $U
    $mine[0] = request::get("u")
    $gn = w_global($G, $N, request::get("u"))
    $g = $G
    $g[0] = request::get("u")          // a request's own copy of a top-level value
    return response::json([count($gn[0]), $gn[0][0], $gn[1]["k"][0], $mine[0], $U[0],
                           counted(w_push($P, $Q, $S, request::get("u"))), $g[0], $G[0]])
})
route("GET", "/r", fn() use ($U) => response::json(["const" => LIST[0], "global" => $G[0], "nested" => $N["k"][0],
                                                    "use" => $U[0], "count" => count($G),
                                                    "pushed" => count($P), "nested_pushed" => count($Q["k"]), "popped" => count($S)]))
LK
WANT='{"const":"c0","global":"g0","nested":"n0","use":"u0","count":1,"pushed":1,"nested_pushed":1,"popped":2}'
fail=0
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
before="$(curl -s -m 3 "localhost:$PORT/r")"
w="$(curl -s -m 3 "localhost:$PORT/w?u=LEAK")"
after="$(curl -s -m 3 "localhost:$PORT/r")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
# The writing request must see its own writes; the captured value stays what it was.
[ "$w" = '[2,"LEAK","LEAK","LEAK","u0",5,"LEAK","g0"]' ] || { echo "  FAIL the writing request did not see its own writes: $w $(grep -m1 ERROR "$TMP/log.txt" | cut -c1-160)"; fail=1; }
if [ "$before" = "$WANT" ] && [ "$after" = "$WANT" ]; then echo "  OK   the next request reads the declared values"
else echo "  FAIL before=$before"; echo "       after =$after"; fail=1; fi

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
# A function or a handler that writes to a top-level variable must not load either.
cat > "$TMP/bad4.lk" <<'LK'
$G = ["g0"]
function w() { $G[0] = request::get("u") }
route("GET", "/ok", fn() => "served")
LK
cat > "$TMP/bad5.lk" <<'LK'
$P = ["p0"]
route("GET", "/x", function() { push($P, request::get("u")) })
route("GET", "/ok", fn() => "served")
LK
for app in bad1 bad2 bad3 bad4 bad5; do
    ( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 $app.lk > "$TMP/bad.txt" 2>&1 ) & pid=$!
    # Wait until the server either exits (refused at load) or serves — not a fixed time: a slow
    # build (sanitizers) had not written its load error yet after a fixed 0.8 s.
    got=""
    for i in $(seq 1 200); do
        kill -0 "$pid" 2>/dev/null || break
        got="$(curl -s -m 1 "localhost:$PORT/ok")"; [ "$got" = "served" ] && break
        sleep 0.1
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" != "served" ] && grep -qE "captures the value and cannot change it|a variable outside it" "$TMP/bad.txt"; then echo "  OK   refused at load: $app"
    else echo "  FAIL $app: a handler writing to a captured variable was loaded (got=[$got]): $(head -2 "$TMP/bad.txt" | tr '\n' ' ' | cut -c1-200)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: requests are isolated from each other" || { echo "FAIL: request isolation"; exit 1; }
