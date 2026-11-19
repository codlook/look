#!/usr/bin/env bash
set +e
cd /look/cpp
echo "=== differential regresyon ==="
bash tests/differential_test.sh /look/cpp/build/lk /look/cpp/build/lk-fcgi 2>&1 | grep -E '^PASS|^FAIL'
echo "=== eszamanli functional (izolasyon: her istek n=6, crash yok) ==="
./build/lk-fcgi --mode http --port 7712 --workers 4 build/cacherace.lk >/tmp/cf.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:7712/health 2>/dev/null | grep -q ok && break; sleep 0.3; done
curl -s --max-time 2 http://127.0.0.1:7712/set >/dev/null
res=$(for r in $(seq 1 10); do for c in $(seq 1 30); do curl -s --max-time 3 http://127.0.0.1:7712/mut; echo; done & done; wait | sort | uniq -c)
echo "$res"
echo "(izole ise HEP n=6: her istek kendi kopyasini push eder, 5+1; paylasim olsa artan/carpik olurdu)"
kill $pid 2>/dev/null
