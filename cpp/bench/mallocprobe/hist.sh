#!/bin/bash
cd /look/cpp/bench/mallocprobe
gcc -O2 -shared -fPIC mc.c -o mc.so -ldl 2>&1 || exit 1
export LD_PRELOAD=/look/cpp/bench/mallocprobe/mc.so
/look/cpp/build/lk-fcgi --mode http --port 7605 app.lk >/tmp/o.log 2>/tmp/e.log &
SRV=$!; unset LD_PRELOAD; sleep 1.2
dumpline(){ kill -USR1 $SRV; sleep 0.3; grep '^HIST' /tmp/e.log | tail -1; }
run(){ for i in $(seq 1 $2); do curl -s "http://127.0.0.1:7605$1" >/dev/null; done; }
N=3000
run /empty 200; A="$(dumpline)"
run /empty $N;  B="$(dumpline)"
kill $SRV 2>/dev/null
echo "A: $A"
echo "B: $B"
# per-request bucket deltas
echo "$A" "$B" | awk -v n=$N '{
  # fields: A total=$2, buckets $4..$12 as label:val ; B total=$14, buckets $16..$24
  split($0,f," ");
  # parse "label:val" tokens: collect numbers after colon in order for A then B
  na=0; for(i=1;i<=NF;i++){ if($i ~ /:/){ split($i,kv,":"); na++; if(na<=9) av[na]=kv[2]; else bv[na-9]=kv[2]; } }
  lbl[1]="<=16";lbl[2]="17-32";lbl[3]="33-48";lbl[4]="49-64";lbl[5]="65-128";lbl[6]="129-256";lbl[7]="257-1K";lbl[8]="1K-4K";lbl[9]=">4K";
  printf "per-req by size class (malloc/req):\n";
  tot=0;
  for(k=1;k<=9;k++){ d=(bv[k]-av[k])/n; tot+=d; printf "  %-8s : %6.1f\n",lbl[k],d; }
  printf "  %-8s : %6.1f\n","TOTAL",tot;
}'
