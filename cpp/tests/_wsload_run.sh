#!/usr/bin/env bash
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
cd /look/cpp
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 log_path=/tmp/tsan_ws"
./build-tsan/lk-fcgi --mode http --port 7711 --workers 4 build/wsload.lk >/tmp/srv.log 2>&1 &
SRV=$!
for i in $(seq 1 30); do curl -s --max-time 1 http://127.0.0.1:7711/health 2>/dev/null | grep -q ok && break; sleep 0.4; done
echo "server up, load basliyor..."
python3 tests/_wsload.py 2>&1 | tail -2
sleep 1
# graceful kapat (s_cv shutdown gurultusunu ayirmak icin once normal, sonra rapor)
kill -TERM $SRV 2>/dev/null; sleep 2; kill -9 $SRV 2>/dev/null
echo "=== TSan data-race raporlari ==="
cat /tmp/tsan_ws.* 2>/dev/null > /tmp/tsan_all.txt; cp /tmp/tsan_all.txt /look/cpp/build/tsan_ws_report.txt
races=$(grep -c "data race" /tmp/tsan_all.txt 2>/dev/null)
echo "data race sayisi: ${races:-0}"
# her race icin: konum + hangi paylasimli yapi (allocation/access)
grep -iE "WARNING: ThreadSanitizer: data race|Location is|ws_clients|ws_bufs|WsRegistry|WsConnection|look::s_cv|frag_buf|write_mutex|#0 .*http_server|#0 .*websocket" /tmp/tsan_all.txt 2>/dev/null | head -40
