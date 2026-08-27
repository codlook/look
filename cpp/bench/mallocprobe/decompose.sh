#!/bin/bash
cd /look/cpp/bench/mallocprobe
gcc -O2 -shared -fPIC mc.c -o mc.so -ldl 2>&1 || exit 1
export LD_PRELOAD=/look/cpp/bench/mallocprobe/mc.so
/look/cpp/build/lk-fcgi --mode http --port 7604 app.lk >/tmp/o.log 2>/tmp/e.log &
SRV=$!; unset LD_PRELOAD; sleep 1.2
rt(){ kill -USR1 $SRV; sleep 0.3; grep MALLOC_TOTAL /tmp/e.log | tail -1 | cut -d= -f2; }
N=2000
run(){ local url=$1 hh=$2 n=$3; for i in $(seq 1 $n); do curl -s $hh "http://127.0.0.1:7604$url" >/dev/null; done; }
H12="-H x1:1 -H x2:2 -H x3:3 -H x4:4 -H x5:5 -H x6:6 -H x7:7 -H x8:8 -H x9:9 -H xa:a -H xb:b -H xc:c"
Q9="?a=1&b=2&c=3&d=4&e=5&f=6&g=7&h=8&i=9"
run /empty "" 200; P=$(rt)                 # warmup
run /empty "" $N;    A=$(rt)               # base per-req = (A-P)/N
run "/empty$Q9" "" $N; B=$(rt)            # 9-param per-req = (B-A)/N
run /empty "$H12" $N;  C=$(rt)            # 12-header per-req = (C-B)/N
kill $SRV 2>/dev/null
awk -v p=$P -v a=$A -v b=$B -v c=$C -v n=$N 'BEGIN{
  base=(a-p)/n; q9=(b-a)/n; h12=(c-b)/n;
  printf "base per-req (no query, ~3 default hdr): %.1f malloc/req\n", base;
  printf "9-param  per-req: %.1f  -> per query param: %.2f malloc\n", q9, (q9-base)/9;
  printf "12-hdr   per-req: %.1f  -> per extra header: %.2f malloc\n", h12, (h12-base)/12;
}'
