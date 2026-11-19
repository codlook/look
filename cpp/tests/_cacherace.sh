#!/usr/bin/env bash
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
cd /look/cpp
export TSAN_OPTIONS="halt_on_error=0"
cmake --build build-tsan --target look-fcgi -j8 >/tmp/b.log 2>&1 || { echo BUILD_FAIL; tail -12 /tmp/b.log; exit 1; }
./build-tsan/lk-fcgi --mode http --port 7711 --workers 4 build/cacherace.lk >/tmp/cr.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:7711/health 2>/dev/null | grep -q ok && break; sleep 0.3; done
curl -s --max-time 2 http://127.0.0.1:7711/set >/dev/null   # cache'i doldur
for r in $(seq 1 20); do for c in $(seq 1 30); do curl -s --max-time 3 http://127.0.0.1:7711/mut >/dev/null & done; wait; done
sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null
races=$(grep -c "WARNING: ThreadSanitizer: data race" /tmp/cr.log)
echo "=== data race sayisi = $races ==="
grep -A8 "WARNING: ThreadSanitizer: data race" /tmp/cr.log | grep -iE "vector|push_back|cache|_M_|write of|read of|CacheStore" | head -6
