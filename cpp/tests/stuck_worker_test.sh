#!/bin/bash
# Fiber kipinde bölünemeyen uzun bir yerleşik (büyük json::decode) öteki istekleri bekletmez.
# ESKİ: SO_REUSEPORT'ta çekirdek yeni bağlantıyı işçinin meşgul olup olmadığına bakmadan bir
# sokete atar; o işçi 0,3 sn'lik decode'dayken sokete düşen /ok decode bitene dek kabul edilmiyordu
# (16 CPU'da ölçüldü: p99 314 ms; aynı işçiye bağlı keep-alive bağlantısı p99 119 ms). VM döngüsü
# 1 ms'de sırayı bıraktığı için yalnız yerleşiklerde. Şimdi:
#   - gözetleyici (http_server.cpp Impl::watched) takılan işçinin bekleyen bağlantılarını boştaki
#     işçiye boşalttırır (p99 2,3 ms);
#   - büyük json::decode ortak havuzda çözülür (web_stdlib.cpp), işçi öteki istekleri sürdürür
#     (keep-alive p99 1,4 ms).
# Eşikler cömert (yük altındaki CI'da kararlı): eski davranış her bekleyişte bir decode süresi
# (yeni bağlantı ≥ 330 ms, keep-alive 140–240 ms ölçüldü) bekletiyordu; yenisi ≤ 8 ms / ≤ 0,4 ms
# (CPU yükü altında da) → eşik 150 ms / 60 ms.
# LOOK_TEST_NO_TIMING=1: yalnız doğruluk (TSan — yavaş).
# Usage: stuck_worker_test.sh <lk-fcgi> [port]
set -u
LKF="$1"; P="${2:-7690}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T=$(mktemp -d); pid=""
trap '{ touch "$T/stop"; [ -n "$pid" ] && kill -9 $pid; jobs -p | xargs -r kill; wait; } 2>/dev/null; rm -rf "$T"' EXIT
( sleep 240; echo "FAIL: stuck worker test timed out (240 s)"; kill -TERM $$ ) 2>/dev/null &
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
timing=1; [ "${LOOK_TEST_NO_TIMING:-}" = 1 ] && timing=0
cat > "$T/app.lk" <<'LK'
use json
use string
$parts = []
for ($i = 0; $i < 250000; $i++) { push($parts, "{\"id\":" . $i . ",\"name\":\"user " . $i . "\",\"tags\":[\"a\",\"b\"]}") }
$BIG = "[" . string::join($parts, ",") . "]"
route("GET", "/ok", fn() => response::text("ok"))
route("GET", "/decode", function() { return response::text("n=" . count(json::decode($BIG))) })
LK
# 8 işçi, 2'si decode'da: boştaki işçi her zaman var (CI makinesinin CPU sayısından bağımsız).
LOOK_FIBER_DISPATCH=1 LOOK_MEMORY_LIMIT=off "$LKF" --mode http --port "$P" --workers 8 "$T/app.lk" > "$T/srv.log" 2>&1 & pid=$!
for i in $(seq 1 150); do [ "$(curl -s -m 1 "http://127.0.0.1:$P/ok")" = ok ] && break; sleep 0.2; done
d=$(curl -s -m 60 "http://127.0.0.1:$P/decode")
check "the long builtin works ($d)" "$( [ "$d" = n=250000 ] && echo ok)" "got '$d'"
: > "$T/decodes"
for g in 1 2; do ( while [ ! -e "$T/stop" ]; do curl -s -m 60 "http://127.0.0.1:$P/decode" >> "$T/decodes"; echo >> "$T/decodes"; done ) & done
sleep 1
# yeni bağlantılar
: > "$T/nc"; bad=0
for i in $(seq 1 100); do
    r=$(curl -s -m 30 -w ' %{time_total}' "http://127.0.0.1:$P/ok"); echo "$r" >> "$T/nc"
    [ "${r%% *}" = ok ] || bad=$((bad + 1))
done
# keep-alive: 20 bağlantı × 10 istek (aynı bağlantı); büyük decode havuzda → işçi bekletilmez
: > "$T/ka"
for c in $(seq 1 20); do
    u=""; for r in $(seq 1 10); do u="$u -o /dev/null http://127.0.0.1:$P/ok"; done   # -o her adrese ayrı
    curl -s -m 60 -w '%{http_code} %{time_total}\n' $u >> "$T/ka"
done
# Decode'lar gerçekten yanıt verdi mi (TSan'da yavaş: en çok 120 sn bekle)
for i in $(seq 1 1200); do [ "$(grep -c "n=250000" "$T/decodes")" -ge 2 ] && break; sleep 0.1; done
touch "$T/stop"
nd=$(grep -c "n=250000" "$T/decodes")
check "decodes ran meanwhile ($nd)" "$( [ "$nd" -ge 2 ] && echo ok)" "only $nd"
check "every /ok on a new connection was answered (100)" "$( [ $bad = 0 ] && echo ok)" "$bad not answered"
kab=$(grep -vc '^200 ' "$T/ka")
check "every /ok on a keep-alive connection was answered (200)" "$( [ "$kab" = 0 ] && echo ok)" "$kab not answered"
if [ $timing = 1 ]; then
    mx=$(awk '{print $2*1000}' "$T/nc" | sort -n | tail -1)
    slow=$(awk '$2 > 0.15' "$T/nc" | wc -l)
    check "no /ok on a new connection waited for a decode (slowest ${mx} ms)" "$( [ "$slow" = 0 ] && echo ok)" "$slow of 100 over 150 ms"
    kmx=$(awk '{print $2*1000}' "$T/ka" | sort -n | tail -1)
    kslow=$(awk '$2 > 0.06' "$T/ka" | wc -l)
    check "no /ok on a keep-alive connection waited for a decode (slowest ${kmx} ms)" "$( [ "$kslow" = 0 ] && echo ok)" "$kslow of 200 over 60 ms"
fi
kill -0 $pid 2>/dev/null; check "the server is alive" "$( [ $? = 0 ] && echo ok)" "it died"
[ $fail = 0 ] && echo "PASS: a long builtin does not hold other requests" || echo "FAIL: a long builtin held other requests"
exit $fail
