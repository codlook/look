#!/usr/bin/env bash
# Request isolation. Values created while the script is set up (const arrays, top-level
# arrays, arrays captured by a route or by a closure stored in a variable) were handed to
# every request without a copy. Arrays are shared by reference, so one request writing into
# such an array in place changed it for every later request — another user's request.
# With several workers the same vector was also written concurrently.
# One request writes through every path; the next request must read the declared values.
# Usage: request_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7714}"
cat > "$TMP/app.lk" <<'LK'
use json
const { LIST = ["c0"], DEEP = ["k" => ["d0"]] }
$G = ["g0"]
$U = ["u0"]
$M = ["m0"]
function make_reader($box) { return fn() => $box }
$reader = make_reader(["b0"])
function w_const($u)  { $l = LIST; $l[0] = $u; $d = DEEP; $t = $d["k"]; $t[0] = $u; return 1 }
function w_global($u) { $g = $G; $g[0] = $u; return 1 }
function w_arg($x, $u) { $x[0] = $u; return 1 }
function w_box($u)    { $b = $reader(); $b[0] = $u; return 1 }
before_route(fn() use ($M) => w_arg($M, request::get("u") ?? "m0"))
route("GET", "/w", fn() use ($U) => response::json([w_const(request::get("u")), w_global(request::get("u")),
                                                    w_arg($U, request::get("u")), w_box(request::get("u"))]))
route("GET", "/r", fn() use ($U, $M) => response::json(["const" => LIST[0], "nested" => DEEP["k"][0], "global" => $G[0],
                                                        "use" => $U[0], "closure" => $reader()[0]]))
LK
WANT='{"const":"c0","nested":"d0","global":"g0","use":"u0","closure":"b0"}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    before="$(curl -s -m 3 "localhost:$PORT/r")"
    w="$(curl -s -m 3 "localhost:$PORT/w?u=LEAK")"
    after="$(curl -s -m 3 "localhost:$PORT/r")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$w" = "[1,1,1,1]" ] || { echo "  FAIL [$mode] the writing request did not run: $w $(grep -m1 ERROR "$TMP/log.txt" | cut -c1-160)"; fail=1; }
    if [ "$before" = "$WANT" ] && [ "$after" = "$WANT" ]; then echo "  OK   [$mode] the next request reads the declared values"
    else echo "  FAIL [$mode] before=$before"; echo "       [$mode] after =$after"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: requests are isolated from each other" || { echo "FAIL: request isolation"; exit 1; }
