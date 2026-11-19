#!/bin/bash
# Memory budget (include/look/mem_budget.h): one request cannot kill the whole process.
# OLD: a single request growing without limit killed the server in 1 s in a 512 MB container (rc=137).
#   1) request budget: the growing request gets 500 + a clear error, the server lives, other requests get answers
#   2) process limit: at 90% a new request gets 503; at 100% the request using the MOST memory ends with 503
#      (not the one doing the allocation). The order is built with file signals, NOT with waiting (stable on a loaded machine).
#   3) a request that releases data allocated by another request gains no extra budget (the counter does not go negative)
#   4) invalid LOOK_MEMORY_LIMIT: says why, does not start
#   5) the memory stop cannot be swallowed by try/catch
#   6) concatenation and the intermediate value text at full size: 1 MiB . "y" is 1 MiB (OLD: 2 MiB)
#   7) command line: no default limit, no request budget; if LOOK_MEMORY_LIMIT is given, a process limit
#   8) the counter does not drift: allocation and release are counted with the same size (even if the request's accounting is written in batches) —
#      20 × 100 MiB in one request, 150 requests × 100 MiB in a row: if drift accumulated a request would get 500 / 503
# Usage: memory_budget_test.sh <lk-fcgi> [base-port] [lk]   (needs curl; LOOK_FIBER_DISPATCH passes through from the environment:
#        the Linux --mode http default is fiber, LOOK_FIBER_DISPATCH=0 for the pool)
set -u
LKF="$1"; P="${2:-7940}"; LK="${3:-$(dirname "$LKF")/lk}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""
trap '{ [ -n "$pid" ] && kill -9 $pid; jobs -p | xargs -r kill; wait; } 2>/dev/null; rm -rf "$T"' EXIT
# A stuck test goes red within minutes (the EXIT trap kills the server).
( sleep 240; echo "FAIL: memory budget test timed out (240 s)"; kill -TERM $$ ) 2>/dev/null &
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
mkdir -p "$T/sig"
cat > "$T/app.lk" <<LK
use string
use array
use date
use cache
use file
\$SIG = "$T/sig/"
LK
cat >> "$T/app.lk" <<'LK'
function spin($us) { $t = date::mono_us(); while (date::mono_us() - $t < $us) { } }
// Signal: wait until the test creates the file (at most 30 s — the test is also protected by its own time limit)
function wait_for($name) {
    $t = date::mono_us()
    while (!file::exists($SIG . $name) && date::mono_us() - $t < 30000000) { spin(5000) }
}
// hold n MiB (each element exactly 1 MiB), then write "<name>.ready", hold until "<name>.go" arrives
function fill($n, $name) {
    $a = []
    for ($i = 0; $i < $n; $i++) { $a = array::push($a, string::repeat("x", 1048576)) }
    if ($name != "") { file::put($SIG . $name . ".ready", "1"); wait_for($name . ".go") }
    return count($a)
}
route("GET", "/ok", fn() => response::text("ok"))
route("GET", "/grow", function() { $a = []; while (true) { $a = array::push($a, string::repeat("x", 1048576) . count($a)) } })
route("GET", "/hold", function() { return response::text("held " . fill(int(request::get("mb")), request::get("name"))) })
// C: m1 MiB, write "c.ready", wait for "c.go", then m2 MiB more (that is what crosses the process limit)
route("GET", "/climb", function() {
    $a = []
    for ($i = 0; $i < int(request::get("m1")); $i++) { $a = array::push($a, string::repeat("y", 1048576)) }
    file::put($SIG . "c.ready", "1")
    wait_for("c.go")
    for ($i = 0; $i < int(request::get("m2")); $i++) { $a = array::push($a, string::repeat("z", 1048576)) }
    return response::text("climbed " . count($a))
})
route("GET", "/stash", function() {
    $a = []
    for ($i = 0; $i < 90; $i++) { $a = array::push($a, string::repeat("s", 1048576)) }
    cache::set("big", $a, 600)
    return response::text("stashed")
})
route("GET", "/unstash-then-grow", function() {
    cache::delete("big")
    return response::text("grew " . fill(200, ""))
})
// try/catch cannot swallow the stop: if it did the loop would spin forever (curl timeout)
route("GET", "/catchgrow", function() {
    $a = []
    while (true) { try { $a = array::push($a, string::repeat("x", 1048576)) } catch ($e) { } }
})
// 100 × (1 MiB . "y") and 100 × "{1 MiB}y": at full size 100 MiB < 128 MiB budget (OLD: 200 MiB → stopped)
route("GET", "/concat", function() {
    $a = []
    $s = string::repeat("x", 1048576)
    for ($i = 0; $i < 100; $i++) { $a = array::push($a, $s . "y") }
    return response::text("concat " . count($a))
})
route("GET", "/interp", function() {
    $a = []
    $s = string::repeat("x", 1048576)
    for ($i = 0; $i < 100; $i++) { $a = array::push($a, "{$s}y") }
    return response::text("interp " . count($a))
})
// allocate 100 MiB, release — 20 times (request budget 128 MiB): if the accounting drifts the request stops
route("GET", "/churn", function() {
    for ($k = 0; $k < int(request::get("k")); $k++) {
        $a = []
        for ($i = 0; $i < 100; $i++) { $a = array::push($a, string::repeat("c", 1048576)) }
        $a = []
    }
    return response::text("churned")
})
LK
start() { # <limit> <port>
    LOOK_MEMORY_LIMIT="$1" "$LKF" --mode http --port "$2" --workers 6 "$T/app.lk" > "$T/srv.log" 2>&1 & pid=$!
    for i in $(seq 1 50); do [ "$(curl -s -m 1 "http://127.0.0.1:$2/ok")" = ok ] && return 0; sleep 0.2; done
    echo "  FAIL server did not start"; cat "$T/srv.log"; fail=1; return 1
}
stop() { kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""; }
code() { curl -s -m "${2:-30}" -o "$T/b.$$.$RANDOM" -w '%{http_code}' "$1"; }
# Wait until the signal file arrives (at most 30 s)
await() { for i in $(seq 1 300); do [ -e "$T/sig/$1" ] && return 0; sleep 0.1; done; echo "  FAIL no signal $1"; fail=1; return 1; }

echo "== 1) a request that grows without end"
if start 256MiB $P; then
    check "the startup log states the limits" "$(grep -q 'memory limit 256 MiB (LOOK_MEMORY_LIMIT), 128 MiB per request' "$T/srv.log" && echo ok)" "$(grep -i 'memory limit' "$T/srv.log")"
    ( code "http://127.0.0.1:$P/grow" 60 > "$T/grow.code" ) & g1=$!
    # Is a response COMING (speed was measured separately). In fiber mode a request that lands on the same thread as the growing request
    # waits for it (pre-existing fiber ordering behavior; ~6 s under TSan) — the time is set accordingly.
    sleep 0.3; mid=$(curl -s -m 30 "http://127.0.0.1:$P/ok")
    wait $g1   # curl only: a bare wait also waits for the server (hangs)
    check "the growing request ends with 500 ($(cat "$T/grow.code"))" "$( [ "$(cat "$T/grow.code")" = 500 ] && echo ok)" "code $(cat "$T/grow.code")"
    check "the log says why" "$(grep -q 'the request would use more than 128 MiB of memory and was stopped' "$T/srv.log" && echo ok)" "$(grep -E 'ERROR' "$T/srv.log" | tail -2)"
    check "the server answered during it ('$mid') and after" "$( [ "$mid" = ok ] && [ "$(curl -s -m 5 "http://127.0.0.1:$P/ok")" = ok ] && echo ok)" "during '$mid'"
    kill -0 $pid 2>/dev/null; check "the server is alive" "$( [ $? = 0 ] && echo ok)" "it died"

    echo "== 5) try/catch cannot swallow the stop"
    c5=$(code "http://127.0.0.1:$P/catchgrow" 30)
    check "a request that catches the error in a loop still ends with 500 ($c5)" "$( [ "$c5" = 500 ] && echo ok)" "got $c5 (000 = it never ended)"

    echo "== 6) a joined string takes its own size"
    r6=$(curl -s -m 30 "http://127.0.0.1:$P/concat"); r7=$(curl -s -m 30 "http://127.0.0.1:$P/interp")
    check "100 x (1 MiB . \"y\") fits in 128 MiB ($r6)" "$( [ "$r6" = "concat 100" ] && echo ok)" "got '$r6'"
    check "100 x \"{1 MiB}y\" fits in 128 MiB ($r7)" "$( [ "$r7" = "interp 100" ] && echo ok)" "got '$r7'"
    stop
fi

echo "== 2) the process limit: 503 near it, the biggest request ends at it"
P2=$((P + 1))
if start 300MiB $P2; then   # request budget 150 MiB; 90% = 270 MiB
    ( code "http://127.0.0.1:$P2/hold?mb=130&name=a" 60 > "$T/a.code" ) & ja=$!   # A: holds 130 MiB
    await a.ready
    ( code "http://127.0.0.1:$P2/hold?mb=60&name=b" 60 > "$T/b.code" ) & jb=$!    # B: 60 MiB
    await b.ready
    ( code "http://127.0.0.1:$P2/climb?m1=85&m2=35" 60 > "$T/c.code" ) & jc=$!    # C: 85 MiB (total >270)
    await c.ready
    near=$(code "http://127.0.0.1:$P2/ok" 5)
    touch "$T/sig/c.go"            # C +35 MiB → total >300: the largest (A) stops
    wait $jc
    wait $ja
    touch "$T/sig/b.go"
    wait $jb
    touch "$T/sig/a.go"            # (so it does not hang if A did not stop)
    after=$(code "http://127.0.0.1:$P2/ok" 5)
    check "near the limit a new request gets 503 (got $near)" "$( [ "$near" = 503 ] && echo ok)" "got $near"
    check "at the limit the biggest request (A, 130 MiB) ends with 503: $(cat "$T/a.code")" "$( [ "$(cat "$T/a.code")" = 503 ] && echo ok)" "A got $(cat "$T/a.code")"
    check "the request that crossed the limit (C) finishes: $(cat "$T/c.code")" "$( [ "$(cat "$T/c.code")" = 200 ] && echo ok)" "C got $(cat "$T/c.code")"
    check "B finishes: $(cat "$T/b.code")" "$( [ "$(cat "$T/b.code")" = 200 ] && echo ok)" "B got $(cat "$T/b.code")"
    check "the log names the biggest request and says it stopped midway" "$(grep -q 'reached its memory limit (300 MiB, LOOK_MEMORY_LIMIT) and this request, using the most memory (130 MiB), was stopped midway' "$T/srv.log" && echo ok)" "$(grep ERROR "$T/srv.log" | tail -2)"
    check "afterwards requests are served again ($after)" "$( [ "$after" = 200 ] && echo ok)" "got $after"
    stop
fi

echo "== 3) freeing another request's data does not raise the budget"
P3=$((P + 2))
if start 400MiB $P3; then   # request budget 200 MiB
    s=$(curl -s -m 30 "http://127.0.0.1:$P3/stash")
    g=$(code "http://127.0.0.1:$P3/unstash-then-grow" 60)
    check "stash ($s), then a request frees it and grows 200 MiB: stopped at its own 200 MiB ($g)" "$( [ "$g" = 500 ] && grep -q 'more than 200 MiB' "$T/srv.log" && echo ok)" "got $g"
    stop
fi

echo "== 4) an invalid LOOK_MEMORY_LIMIT"
LOOK_MEMORY_LIMIT=1GB timeout 10 "$LKF" --mode http --port $((P + 3)) --workers 2 "$T/app.lk" > "$T/bad.log" 2>&1; rc=$?
check "exit 1 with the reason (rc=$rc)" "$( [ $rc = 1 ] && grep -q 'LOOK_MEMORY_LIMIT=1GB is not a size' "$T/bad.log" && echo ok)" "$(head -2 "$T/bad.log")"

echo "== 8) the counters do not drift"
P8=$((P + 4))
if start 256MiB $P8; then   # request budget 128 MiB; 90% = 230 MiB
    # TSan (LOOK_TEST_NO_TIMING=1) is very slow: light (tests the race, not the drift)
    K8=20; N8=150; [ "${LOOK_TEST_NO_TIMING:-}" = 1 ] && { K8=3; N8=10; }
    c8=$(code "http://127.0.0.1:$P8/churn?k=$K8" 120)
    check "one request allocates and frees 100 MiB $K8 times ($c8)" "$( [ "$c8" = 200 ] && echo ok)" "got $c8: $(grep ERROR "$T/srv.log" | tail -1)"
    bad8=0
    for i in $(seq 1 $N8); do r=$(code "http://127.0.0.1:$P8/hold?mb=100&name=" 30); [ "$r" = 200 ] || bad8=$((bad8 + 1)); done
    check "$N8 requests of 100 MiB in a row all finish ($bad8 refused)" "$( [ $bad8 = 0 ] && echo ok)" "$bad8 not 200: $(grep ERROR "$T/srv.log" | tail -1)"
    check "afterwards a request is not refused" "$( [ "$(code "http://127.0.0.1:$P8/ok" 5)" = 200 ] && echo ok)" "refused"
    stop
fi

echo "== 7) the command line: no limit by default, LOOK_MEMORY_LIMIT when given"
if [ -x "$LK" ]; then
    cat > "$T/big.lk" <<'LK'
use string
use array
$a = []
for ($i = 0; $i < 300; $i++) { $a = array::push($a, string::repeat("x", 1048576)) }
print("held " . count($a))
LK
    cat > "$T/catch.lk" <<'LK'
use string
use array
$a = []
while (true) { try { $a = array::push($a, string::repeat("x", 1048576)) } catch ($e) { } }
LK
    out=$(env -u LOOK_MEMORY_LIMIT timeout 60 "$LK" "$T/big.lk" 2>&1); rc=$?
    check "300 MiB with no setting: no request budget, no default limit (rc=$rc)" "$( [ $rc = 0 ] && [ "$out" = "held 300" ] && echo ok)" "$out"
    out=$(LOOK_MEMORY_LIMIT=200MiB timeout 60 "$LK" "$T/big.lk" 2>&1); rc=$?
    check "LOOK_MEMORY_LIMIT=200MiB stops it with the reason (rc=$rc)" "$( [ $rc = 1 ] && echo "$out" | grep -q 'reached its memory limit (200 MiB, LOOK_MEMORY_LIMIT) and was stopped' && echo ok)" "$out"
    out=$(LOOK_MEMORY_LIMIT=200MiB timeout 60 "$LK" "$T/catch.lk" 2>&1); rc=$?
    check "try/catch cannot swallow it on the command line either (rc=$rc)" "$( [ $rc = 1 ] && echo "$out" | grep -q 'memory limit' && echo ok)" "rc=$rc $out"
    out=$(LOOK_MEMORY_LIMIT=12XB timeout 10 "$LK" "$T/big.lk" 2>&1); rc=$?
    check "an invalid value is refused (rc=$rc)" "$( [ $rc = 1 ] && echo "$out" | grep -q 'LOOK_MEMORY_LIMIT=12XB is not a size' && echo ok)" "$out"
else
    echo "  FAIL lk not found at $LK"; fail=1
fi

[ $fail = 0 ] && echo "PASS: memory budget" || echo "FAIL: memory budget"
exit $fail
