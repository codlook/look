#!/bin/bash
# Sözlüğe push açık hatadır (include/look/value_path.h value_push_not_map). ESKİ: push sözlüğe
# anahtarsız tek değer ekliyordu; sonra eklenen anahtarlar sessizce null okunuyordu.
# Her push yolu: push() (değişken yolu ve ifade), array::push() (yerinde derlenen ve kopyalayan
# yol), iç içe yol, fonksiyon içi, web kurulumu ve istek. Hata satır numarasıyla gelir, sözlüğün
# iç işareti (__assoc__) hiçbir çıktıda görünmez, listeye push değişmez.
# Usage: push_map_error_test.sh <lk> <lk-fcgi> [port]   (needs curl)
set -u
LK="$1"; LKF="$2"; PORT="${3:-7933}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""; trap '{ [ -n "$pid" ] && kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
fail=0
# case <ad> <beklenen ad (push() | array::push())> <beklenen satır> <betik>
case_err() {
    printf '%s\n' "$4" > "$T/c.lk"
    out=$("$LK" "$T/c.lk" 2>&1); rc=$?
    if [ $rc = 0 ] || ! grep -qF "$2 cannot add to a map; use \$m[\$key] = \$value" <<<"$out" \
       || ! grep -qE "Line: $3\b" <<<"$out"; then
        echo "FAIL $1: want '$2 cannot add to a map' at line $3, got rc=$rc:"; printf '%s\n' "$out" | head -8; fail=1
    elif grep -q "__assoc__" <<<"$out"; then
        echo "FAIL $1: the map's internal marker is in the output"; fail=1
    else echo "PASS $1"; fi
}
case_err "push() on a map variable" "push()" 2 '$m = ["a" => 1, "b" => 2]
push($m, 99)
print("not reached")'
case_err "push() inside a function" "push()" 3 'function f() {
    $m = ["a" => 1]
    push($m, 99)
    return $m
}
f()'
case_err "push() on a nested map" "push()" 2 '$d = ["rows" => ["a" => 1]]
push($d["rows"], 99)'
case_err "push() on a map expression" "push()" 1 'print(push(["a" => 1], 99))'
case_err "\$m = array::push(\$m, v)" "array::push()" 3 'use array
$m = ["a" => 1]
$m = array::push($m, 99)'
case_err "array::push into another variable" "array::push()" 3 'use array
$m = ["a" => 1]
$x = array::push($m, 99)'

# yakalanabilir; sözlük değişmeden kalır
printf '%s\n' 'use json
$m = ["a" => 1, "b" => 2]
try { push($m, 99) } catch ($e) { print("caught: " . $e) }
$m["c"] = 3
print(json::encode($m))' > "$T/c.lk"
out=$("$LK" "$T/c.lk" 2>&1)
if grep -qF 'caught: push() cannot add to a map' <<<"$out" && grep -qF '{"a":1,"b":2,"c":3}' <<<"$out" && ! grep -q __assoc__ <<<"$out"
then echo "PASS caught, map unchanged"; else echo "FAIL caught, map unchanged: $out"; fail=1; fi

# listeye push değişmedi
printf '%s\n' 'use array
use json
$a = [1, 2]
push($a, 3)
$a = array::push($a, 4)
$b = array::push($a, 5)
$e = []
push($e, "x")
print(json::encode([$a, $b, $e]))' > "$T/c.lk"
out=$("$LK" "$T/c.lk" 2>&1)
if [ "$out" = '[[1,2,3,4],[1,2,3,4,5],["x"]]' ]; then echo "PASS push on lists"; else echo "FAIL push on lists: $out"; fail=1; fi

# web: istek içinde (hata yanıtı + günlük), kurulumda (başlamaz)
cat > "$T/app.lk" <<'LK'
route("GET", "/p", function() {
    $m = ["a" => 1]
    push($m, 99)
    return response::text("not reached")
})
route("GET", "/ok", function() { $a = [1]; push($a, 2); return response::text("" . count($a)) })
LK
wait_up() { for i in $(seq 1 100); do curl -s --max-time 1 "http://127.0.0.1:$1/health" | grep -q ok && return 0; sleep 0.2; done; return 1; }
"$LKF" --mode http --port "$PORT" --workers 2 "$T/app.lk" >"$T/srv.log" 2>&1 &
pid=$!
if wait_up "$PORT"; then
    code=$(curl -s -o "$T/body" -w '%{http_code}' "http://127.0.0.1:$PORT/p")
    ok=$(curl -s "http://127.0.0.1:$PORT/ok")
    sleep 0.3
    if [ "$code" = 500 ] && grep -qF 'push() cannot add to a map; use $m[$key] = $value (line 3)' "$T/srv.log" && [ "$ok" = 2 ] \
       && ! grep -q __assoc__ "$T/srv.log" "$T/body"
    then echo "PASS web request"; else echo "FAIL web request: code=$code ok=$ok"; tail -5 "$T/srv.log"; fail=1; fi
    route_disable=$(grep -c "route disable" "$T/srv.log")
    [ "$route_disable" = 0 ] || { echo "FAIL web request: route disabled ($route_disable)"; fail=1; }
else echo "FAIL web: server did not start"; cat "$T/srv.log"; fail=1; fi
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""

printf '%s\n' '$m = ["a" => 1]' 'push($m, 99)' 'route("GET", "/", function() { return response::text("up") })' > "$T/setup.lk"
timeout 20 "$LKF" --mode http --port "$PORT" --workers 1 "$T/setup.lk" >"$T/setup.log" 2>&1 &
pid=$!
sleep 2
if curl -s --max-time 1 "http://127.0.0.1:$PORT/" | grep -q up; then
    echo "FAIL web setup: started although setup pushed onto a map"; fail=1
elif grep -qF 'push() cannot add to a map; use $m[$key] = $value (line 2)' "$T/setup.log"; then
    echo "PASS web setup"
else echo "FAIL web setup:"; head -8 "$T/setup.log"; fail=1; fi
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
exit $fail
