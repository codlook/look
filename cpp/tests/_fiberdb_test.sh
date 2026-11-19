#!/usr/bin/env bash
set +e
cd /look/cpp/build
LOOK_FIBER_DISPATCH=1 LOOK_DB_POOL_SIZE=4 ./lk-fcgi --mode http --port 9673 --workers 3 fiberdb.lk >/tmp/fdb.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:9673/health 2>/dev/null | grep -q ok && break; sleep 0.3; done
echo "=== tekil dogruluk (q=2,1 bekleniyor) ==="; curl -s --max-time 2 http://127.0.0.1:9673/q; echo
rss0=$(ps -o rss= -p $pid 2>/dev/null | tr -d ' ')
echo "=== 600 istek, 50 eszamanli (havuz=4) — sizinti->hang, bozulma->yanlis ==="
for round in $(seq 1 12); do
  for c in $(seq 1 50); do curl -s --max-time 4 http://127.0.0.1:9673/q; echo; done
done | sort | uniq -c
rss1=$(ps -o rss= -p $pid 2>/dev/null | tr -d ' ')
echo "=== RSS once=$rss0 sonra=$rss1 KB (buyuk artis=sizinti) ==="
echo "=== fallback/hata log sayisi ==="; grep -icE 'fallback|VM hata|error|exhaust|leak' /tmp/fdb.log
kill $pid 2>/dev/null
