#!/usr/bin/env bash
cd /look/cpp
./build/lk-fcgi --mode http --port 7401 --workers 4 build/manyroutes.lk >/tmp/lt.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:7401/health 2>/dev/null | grep -q ok && break; sleep 0.3; done
python3 tests/_loadtest.py "${1:-run}"
kill $pid 2>/dev/null
