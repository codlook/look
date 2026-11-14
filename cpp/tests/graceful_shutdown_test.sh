#!/bin/bash
# Zarif kapanma (include/look/shutdown.h): SIGTERM'de işlenen istek biter, yeni bağlantı alınmaz,
# boşta bekleyen keep-alive bağlantısı çıkışı geciktirmez, süreç 0 ile çıkar. İkinci sinyal hemen
# çıkarır (1). ESKİ: işleyici yoktu — SIGTERM (systemd stop/restart, Plesk sürüm geçişi) süreci
# anında öldürüyor, yarıdaki istek kesiliyordu.
# Usage: graceful_shutdown_test.sh <lk-fcgi> [base-port]   (needs curl, python3)
set -u
LKF="$1"; BASE="${2:-7960}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""; bg=""
trap '{ [ -n "$pid" ] && kill -9 $pid; [ -n "$bg" ] && kill $bg; wait; } 2>/dev/null; rm -rf "$T"' EXIT
fail=0
cat > "$T/app.lk" <<'LK'
use date
function spin($us) {
    $t = date::mono_us()
    while (date::mono_us() - $t < $us) { }
}
route("GET", "/slow", function() { spin(2000000); return response::text("done") })
route("GET", "/slow10", function() { spin(10000000); return response::text("done") })
route("GET", "/fast", function() { return response::text("ok") })
LK
now_ms() { date +%s%3N; }
wait_exit() { # <pid> <max s> → çıkış kodu (124 = süre doldu)
    local i; for i in $(seq 1 $(( $2 * 10 ))); do kill -0 "$1" 2>/dev/null || { wait "$1"; return $?; }; sleep 0.1; done; return 124
}
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }

for mode in http fcgi; do
    echo "== lk-fcgi ($mode): SIGTERM while a request is running"
    port=$BASE; BASE=$((BASE + 1))
    if [ $mode = http ]; then "$LKF" --mode http --port $port --workers 2 "$T/app.lk" > "$T/srv.log" 2>&1 & pid=$!
    else "$LKF" --port $port --workers 2 "$T/app.lk" > "$T/srv.log" 2>&1 & pid=$!; fi
    q() { if [ $mode = http ]; then curl -s -m "${2:-5}" "http://127.0.0.1:$port$1"
          else timeout "${2:-5}" python3 "$HERE/fcgi_get.py" $port "$1" "$T/app.lk" 2>/dev/null | tail -1; fi; }
    up=""; for i in $(seq 1 50); do [ "$(q /fast 1)" = ok ] && { up=1; break; }; sleep 0.2; done
    [ -n "$up" ] || { echo "  FAIL server did not start"; cat "$T/srv.log"; fail=1; kill -9 $pid; wait $pid 2>/dev/null; pid=""; continue; }

    # 1) uzun istek  2) boşta açık keep-alive bağlantısı (HTTP: bir istekten sonra açık kalır)
    if [ $mode = http ]; then
        curl -s -m 15 -D "$T/hdr" -o "$T/body" "http://127.0.0.1:$port/slow" & bg=$!
        python3 - "$port" <<'PY' > /dev/null 2>&1 &
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET /fast HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(4096)
time.sleep(40)
PY
        idle=$!
    else
        ( q /slow 15 > "$T/body" ) & bg=$!
        idle=""
    fi
    sleep 0.5
    t0=$(now_ms); kill -TERM $pid
    sleep 0.3
    after="$(q /fast 1)"
    # Port hemen bırakılır: eski süreç uzun isteği bitirirken aynı porta yenisi başlar (restart/deploy).
    if [ $mode = http ]; then "$LKF" --mode http --port $port --workers 2 "$T/app.lk" > "$T/srv2.log" 2>&1 & pid2=$!
    else "$LKF" --port $port --workers 2 "$T/app.lk" > "$T/srv2.log" 2>&1 & pid2=$!; fi
    second=""; for i in $(seq 1 20); do [ "$(q /fast 1)" = ok ] && { second=ok; break; }; sleep 0.1; done
    old_alive=$(kill -0 $pid 2>/dev/null && echo yes || echo no)
    kill -9 $pid2 2>/dev/null; wait $pid2 2>/dev/null
    check "a new server takes the port while the old one finishes (old still running: $old_alive)" \
        "$( [ "$second" = ok ] && echo ok)" "second server: $(tail -1 "$T/srv2.log")"
    wait_exit $pid 10; rc=$?; t1=$(now_ms); pid=""
    wait $bg 2>/dev/null; bg=""
    [ -n "$idle" ] && { kill $idle 2>/dev/null; wait $idle 2>/dev/null; }
    el=$((t1 - t0))
    check "the running request finished: '$(cat "$T/body" 2>/dev/null)'" "$( [ "$(cat "$T/body" 2>/dev/null)" = done ] && echo ok)" "body '$(cat "$T/body" 2>/dev/null)'"
    if [ $mode = http ]; then
        check "its response says Connection: close" "$(grep -qi '^connection: close' "$T/hdr" && echo ok)" "$(tr -d '\r' < "$T/hdr" | tr '\n' ' ')"
    fi
    check "no new request after the signal" "$( [ "$after" != ok ] && echo ok)" "a new request got '$after'"
    check "exit code 0 (got $rc)" "$( [ "$rc" = 0 ] && echo ok)" "rc=$rc"
    # Uzun istek ~1,5 sn daha sürüyordu; boşta bağlantı çıkışı 30 sn (boşta süresi) geciktirmemeli.
    check "stopped in ${el} ms (an idle connection does not hold it)" "$( [ $el -lt 5000 ] && echo ok)" "${el} ms"
    check "the log says what happens" "$(grep -q 'SIGTERM: no new connections' "$T/srv.log" && grep -q '\] stopped$' "$T/srv.log" && echo ok)" \
        "$(grep -E 'SIGTERM|stopped' "$T/srv.log" | tr '\n' ' ')"
done

echo "== lk-fcgi (http): a second signal stops at once"
port=$BASE
"$LKF" --mode http --port $port --workers 2 "$T/app.lk" > "$T/srv.log" 2>&1 & pid=$!
up=""; for i in $(seq 1 50); do [ "$(curl -s -m 1 "http://127.0.0.1:$port/fast")" = ok ] && { up=1; break; }; sleep 0.2; done
if [ -n "$up" ]; then
    curl -s -m 15 -o /dev/null "http://127.0.0.1:$port/slow10" & bg=$!
    sleep 0.5; kill -TERM $pid; sleep 0.5
    t0=$(now_ms); kill -INT $pid
    wait_exit $pid 5; rc=$?; t1=$(now_ms); pid=""
    kill $bg 2>/dev/null; wait $bg 2>/dev/null; bg=""
    check "second signal: exit code 1 in $((t1 - t0)) ms" "$( [ "$rc" = 1 ] && [ $((t1 - t0)) -lt 2000 ] && echo ok)" "rc=$rc after $((t1 - t0)) ms"
    check "the log says so" "$(grep -q 'second signal: stopping now' "$T/srv.log" && echo ok)" "$(grep -E 'signal|stopp' "$T/srv.log" | tr '\n' ' ')"
else echo "  FAIL server did not start"; cat "$T/srv.log"; fail=1; kill -9 $pid; pid=""; fi

[ $fail = 0 ] && echo "PASS: graceful shutdown" || echo "FAIL: graceful shutdown"
exit $fail
