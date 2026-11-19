#!/usr/bin/env bash
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
cd /look/cpp
export TSAN_OPTIONS="halt_on_error=0"
LABEL="${1:-run}"
cmake --build build-tsan --target look-fcgi -j8 >/tmp/b.log 2>&1 || { echo "$LABEL BUILD_FAIL"; tail -15 /tmp/b.log; exit 1; }
./build-tsan/lk-fcgi --mode http --port 7710 --workers 4 build/ssepush.lk >/tmp/sse_$LABEL.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:7710/health 2>/dev/null | grep -q ok && break; sleep 0.3; done
python3 tests/_sseload.py 7710 6
sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null
races=$(grep -c "WARNING: ThreadSanitizer: data race" /tmp/sse_$LABEL.log)
echo "=== $LABEL: data race sayisi = $races ==="
grep -B1 -A6 "WARNING: ThreadSanitizer: data race" /tmp/sse_$LABEL.log | grep -iE "close_sse|SseConnection|close_fd|write|read of|write of" | head -6
