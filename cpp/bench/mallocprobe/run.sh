#!/bin/bash
cd /look/cpp/bench/mallocprobe
gcc -O2 -shared -fPIC mc.c -o mc.so -ldl 2>&1 || { echo "so build failed"; exit 1; }
export LD_PRELOAD=/look/cpp/bench/mallocprobe/mc.so
/look/cpp/build/lk-fcgi --mode http --port 7600 app.lk >/tmp/o.log 2>/tmp/e.log &
SRV=$!
unset LD_PRELOAD
sleep 1.2
read_total(){ kill -USR1 $SRV; sleep 0.3; grep MALLOC_TOTAL /tmp/e.log | tail -1 | cut -d= -f2; }
hit(){ local path=$1 n=$2; for i in $(seq 1 $n); do curl -s "http://127.0.0.1:7600$path" >/dev/null; done; }
N=2000
hit /j 200                       # warmup
A=$(read_total)
hit /j $N;    B=$(read_total)
hit /empty $N; C=$(read_total)
hit /nope $N;  D=$(read_total)
kill $SRV 2>/dev/null
awk -v a=$A -v b=$B -v c=$C -v d=$D -v n=$N 'BEGIN{
  printf "JSON  /j     : %.1f malloc/req\n",(b-a)/n;
  printf "empty /empty : %.1f malloc/req\n",(c-b)/n;
  printf "404   /nope  : %.1f malloc/req  (pure framework parse+dispatch)\n",(d-c)/n;
  printf "-> JSON+assoc build over framework: %.1f malloc/req\n",((b-a)-(c-b))/n;
}'
