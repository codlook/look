#!/usr/bin/env bash
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
cd /look/cpp
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null
# fix'li dosyaları TSan'da yeniden derle
rm -f build-tsan/lk-fcgi build-tsan/CMakeFiles/look-fcgi.dir/src/{http_server,vm,http_main}.cpp.o
cmake --build build-tsan --target look-fcgi -j8 >/tmp/b.log 2>&1 && echo "TSAN REBUILD OK" || { echo FAIL; tail -8 /tmp/b.log; exit 1; }
export TSAN_OPTIONS="halt_on_error=0 log_path=/tmp/tsan_ws2"
./build-tsan/lk-fcgi --mode http --port 7712 --workers 4 build/wsload.lk >/tmp/srv2.log 2>&1 &
SRV=$!
for i in $(seq 1 30); do curl -s --max-time 1 http://127.0.0.1:7712/health 2>/dev/null | grep -q ok && break; sleep 0.4; done
sed 's/7711/7712/' tests/_wsload.py > /tmp/wl2.py
python3 /tmp/wl2.py 2>&1 | tail -1
sleep 1; kill -TERM $SRV 2>/dev/null; sleep 2; kill -9 $SRV 2>/dev/null
cat /tmp/tsan_ws2.* 2>/dev/null > /tmp/t2.txt
echo "=== FIX SONRASI data race sayisi: $(grep -c 'data race' /tmp/t2.txt 2>/dev/null) ==="
grep -iE 'SUMMARY.*data race|Location is file descriptor|vm_route_disabled|route_disabled' /tmp/t2.txt 2>/dev/null | head -6
