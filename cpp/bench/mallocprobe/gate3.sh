#!/bin/bash
cd /look/cpp/bench/mallocprobe
cp app.lk /tmp/app.lk
echo 'use http' > /tmp/app.lk
echo 'route("GET","/v", function(){ return response::text("V1") })' >> /tmp/app.lk
/look/cpp/build/lk-fcgi --mode http --port 7611 /tmp/app.lk >/tmp/o.log 2>/tmp/e.log &
SRV=$!; sleep 1.5
echo "before reload: $(curl -s http://127.0.0.1:7611/v)"
# değiştir (hot-reload tetikle)
echo 'use http' > /tmp/app.lk
echo 'route("GET","/v", function(){ return response::text("V2") })' >> /tmp/app.lk
sleep 2.5
echo "after reload:  $(curl -s http://127.0.0.1:7611/v)"
kill $SRV 2>/dev/null
