#!/usr/bin/env bash
# runtime::stats() sayaç guard'ı — lk-fcgi --mode http'ye karşı. Bu sayaçlar SESSİZ-sorun
# göstergeleri (request_count, errors_5xx, latency, db_pool). request_count bir kez zaten
# web'de 0'a kırılmıştı (setup interpreter vs VM yolu). KENDİNDEN-AYIRT-EDİCİ: delta assertion'ları —
# sayaç no-op'a kırılırsa değer DÜZ kalır → RED (dispatch kancası düşerse yakalanır).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7778}"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/runtime_stats_app.lk"
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 0.3; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -f "$HERE/_rstats.db"; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }

"$FCGI" --mode http --port "$PORT" "$APP" >/dev/null 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/ping" 2>/dev/null && break; sleep 0.2; done
B="http://127.0.0.1:$PORT"
field(){ curl -s "$B/stats" 2>/dev/null | grep -oE "\"$1\":[0-9.]+" | grep -oE '[0-9.]+$'; }

fail=0
chk(){ if [ "$2" = "$3" ]; then echo "  OK   $1 ($2)"; else echo "  FAIL $1: $2 beklenen $3"; fail=1; fi; }
chkge(){ if awk "BEGIN{exit !($2 >= $3)}"; then echo "  OK   $1 ($2 >= $3)"; else echo "  FAIL $1: $2 < $3"; fail=1; fi; }

# baseline
r0=$(field request_count)
# bilinen yük: 10 ping + 3 boom(500) + 1 db
for i in $(seq 1 10); do curl -s -o /dev/null "$B/ping"; done
for i in 1 2 3; do curl -s -o /dev/null "$B/boom"; done
curl -s -o /dev/null "$B/db"
r1=$(field request_count)
e=$(field errors_5xx)
lat=$(field latency_last_us)
psize=$(field db_pool_size)

# request_count DELTA: en az 14 arttı (10+3+1 + baseline /stats okumaları) → düz kalırsa (no-op) RED
chkge "request_count delta" "$((r1 - r0))" "14"
chk   "errors_5xx (3x boom)" "$e" "3"
chkge "latency_last_us > 0"  "$lat" "1"
chkge "db_pool_size > 0"     "$psize" "1"
# vm_disabled_routes 0 olmalı (bozuk route yok) — VM sağlık kanaryası
chk   "vm_disabled_routes (VM sağlıklı)" "$(field vm_disabled_routes)" "0"

# ── ALAN SÖZLEŞMESİ: monitor paketi (look-packages/monitor, AYRI repo) bu alan adlarına bağlı.
# Biri yeniden adlandırılırsa (bugün vm_fallbacks→vm_disabled_routes yapıldığı gibi) paket sessizce
# BOŞ metrik verir — eksik metrik yanlış metrikten sinsi (Grafana grafiği boş, kimse fark etmez).
# Bu döngü sözleşmeyi ana repo CI'sında kilitler: core rename → burada RED (paket kırılmadan yakalanır).
for f in uptime_sec request_count route_count working_mb private_mb \
         vm_disabled_routes errors_5xx latency_last_us latency_avg_us db_pool_size db_pool_busy; do
  if [ -n "$(field "$f")" ]; then echo "  OK   sözleşme: $f"; else
    echo "  FAIL sözleşme: '$f' alanı YOK — monitor paketi bu ada bağlı, sessizce kırılır"; fail=1; fi
done

[ $fail = 0 ] && echo "PASS: runtime::stats sayaçları + monitor alan sözleşmesi" || echo "FAIL: runtime::stats"
exit $fail
