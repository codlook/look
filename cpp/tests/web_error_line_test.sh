#!/bin/bash
# Web'de yakalanmamış hata günlüğe SATIRIYLA yazılır (http_main.cpp VmErrorPlace): rota işleyicisi,
# dahil edilen dosyadaki fonksiyon, rotasız betik, kurulum. ESKİ: yalnız mesaj ("Dispatch error: boom").
# Usage: web_error_line_test.sh <lk-fcgi> [port]   (needs curl)
set -u
LKF="$1"; PORT="${2:-7934}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""; trap '{ [ -n "$pid" ] && kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
fail=0
wait_up() { for i in $(seq 1 100); do curl -s --max-time 1 "http://127.0.0.1:$1/health" | grep -q ok && return 0; sleep 0.2; done; return 1; }
want() { if grep -qF "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1: want [$2]"; grep -E "ERROR" "$3" | tail -5; fail=1; fi; }

mkdir -p "$T/lib"
printf '%s\n' 'function explode() {' '    throw "from lib"' '}' > "$T/lib/x.lk"
cat > "$T/app.lk" <<'LK'
use "lib/x.lk"
route("GET", "/throw", function() {
    throw "boom"
})
route("GET", "/undef", function() {
    return response::text($nope)
})
route("GET", "/lib", function() {
    explode()
})
LK
"$LKF" --mode http --port "$PORT" --workers 1 "$T/app.lk" >"$T/srv.log" 2>&1 &
pid=$!
if wait_up "$PORT"; then
    for u in throw undef lib; do curl -s -o /dev/null "http://127.0.0.1:$PORT/$u"; done
    sleep 0.3
    want "route throw" "Dispatch error: boom (line 3)" "$T/srv.log"
    want "undefined variable" "Undefined variable: \$nope (line 6)" "$T/srv.log"
    want "error in an included file" "from lib (line 2 in lib/x.lk)" "$T/srv.log"
else echo "FAIL: server did not start"; cat "$T/srv.log"; fail=1; fi
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""

# Rotasız betik kurulumda bir kez koşar: hata yalnız istekte (?x=1) atılır.
printf '%s\n' 'print("hello")' '$x = request::get("x")' 'if ($x == "1") { throw "script mode" }' > "$T/script.lk"
"$LKF" --mode http --port "$PORT" --workers 1 "$T/script.lk" >"$T/s.log" 2>&1 &
pid=$!
if wait_up "$PORT"; then
    curl -s -o /dev/null "http://127.0.0.1:$PORT/?x=1"; sleep 0.3
    want "script without routes" "script mode (line 3)" "$T/s.log"
else echo "FAIL: script server did not start"; cat "$T/s.log"; fail=1; fi
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""

printf '%s\n' 'route("GET", "/", function() { return response::text("up") })' '' 'throw "setup failed"' > "$T/setup.lk"
timeout 20 "$LKF" --mode http --port "$PORT" --workers 1 "$T/setup.lk" >"$T/st.log" 2>&1
want "setup" "Setup error: setup failed (line 3)" "$T/st.log"
exit $fail
