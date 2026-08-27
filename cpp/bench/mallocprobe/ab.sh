#!/bin/bash
cd /look/cpp/bench/mallocprobe
gcc -O2 -shared -fPIC mc.c -o mc.so -ldl 2>&1 || exit 1
measure(){ # $1=binary label $2=binary path
  export LD_PRELOAD=/look/cpp/bench/mallocprobe/mc.so
  "$2" --mode http --port 7601 app.lk >/tmp/o.log 2>/tmp/e.log &
  local SRV=$!; unset LD_PRELOAD; sleep 1.2
  rt(){ kill -USR1 $SRV; sleep 0.3; grep MALLOC_TOTAL /tmp/e.log | tail -1 | cut -d= -f2; }
  hit(){ for i in $(seq 1 $2); do curl -s "http://127.0.0.1:7601$1" >/dev/null; done; }
  local N=2000
  hit /j 200; local A=$(rt)
  hit /j $N;    local B=$(rt)
  hit /nope $N; local C=$(rt)
  kill $SRV 2>/dev/null; sleep 0.5
  awk -v l="$1" -v a=$A -v b=$B -v c=$C -v n=$N 'BEGIN{
    printf "%-6s  JSON /j = %.1f   404 /nope = %.1f  malloc/req\n", l,(b-a)/n,(c-b)/n }'
}
measure OLD /look/cpp/bench/mallocprobe/lk-fcgi-old
measure NEW /look/cpp/bench/mallocprobe/lk-fcgi-new
