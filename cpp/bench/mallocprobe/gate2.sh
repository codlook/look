#!/bin/bash
MODE=$1
cd /look/cpp/bench/mallocprobe
[ "$MODE" = fiber ] && export LOOK_FIBER_DISPATCH=1
/look/cpp/build/lk-fcgi --mode http --port 7610 app.lk >/tmp/o.log 2>/tmp/e.log &
SRV=$!; sleep 1.5
mkdir -p /tmp/resp; rm -f /tmp/resp/*
pids=""
for i in $(seq 1 60); do
  ( r=$(curl -s "http://127.0.0.1:7610/echo?v=req$i"); echo "$r" > /tmp/resp/$i ) &
  pids="$pids $!"
done
wait $pids           # sadece curl job'ları (sunucuyu değil)
kill $SRV 2>/dev/null
bad=0; checked=0
for i in $(seq 1 60); do
  got=$(cat /tmp/resp/$i 2>/dev/null); checked=$((checked+1))
  [ "$got" != "req$i" ] && { bad=$((bad+1)); [ $bad -le 3 ] && echo "MISMATCH: req$i -> '$got'"; }
done
echo "MODE=${MODE:-worker-pool}: checked=$checked mismatches=$bad"
