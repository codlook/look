#!/usr/bin/env bash
# Sequential, isolated timing of the LOOK / Node / PHP micro-benchmark suites + cross-language
# checksum equivalence guard. One process at a time (no contention) — best-of-3, keep MIN warm_ms.
# Fairness lock (see BENCHMARK.md): Node 22 default (JIT on), PHP 8.3 with OPcache + JIT on.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # repo root
LK="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp:/look/cpp -w /look/cpp/bench/micro/look look-build /look/cpp/build/lk run.lk"
ND="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:22-alpine node run.js"
PHP_JIT="-d zend_extension=opcache.so -d opcache.enable_cli=1 -d opcache.jit_buffer_size=128M -d opcache.jit=tracing"
PH="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/php:/app -w /app php:8.3-cli php $PHP_JIT run.php"

run3() {  # command... -> prints "name,checksum,min_ms" per benchmark (min over 3 runs)
  local out1 out2 out3
  out1=$(eval "$1" 2>/dev/null); out2=$(eval "$1" 2>/dev/null); out3=$(eval "$1" 2>/dev/null)
  paste -d'|' <(echo "$out1") <(echo "$out2") <(echo "$out3") | awk -F'|' '
    {
      split($1,a,","); split($2,b,","); split($3,c,",");
      name=a[1]; chk=a[2]; m=a[3]+0;
      if (b[3]+0<m) m=b[3]+0; if (c[3]+0<m) m=c[3]+0;
      printf "%s,%s,%.3f\n", name, chk, m;
    }'
}

echo "=== running LOOK suite (best-of-3, isolated) ==="
LOOK_OUT=$(run3 "$LK"); echo "$LOOK_OUT"
echo "=== running Node suite (Node 22, best-of-3, isolated) ==="
NODE_OUT=$(run3 "$ND"); echo "$NODE_OUT"
echo "=== running PHP suite (8.3 + OPcache + JIT, best-of-3, isolated) ==="
PHP_OUT=$(run3 "$PH"); echo "$PHP_OUT"

echo
echo "=== comparison (checksum MUST match across all three = equivalence guard) ==="
printf "%-22s %12s %12s %12s %10s %10s %6s\n" "benchmark" "LOOK_ms" "Node_ms" "PHP_ms" "LOOK/Node" "LOOK/PHP" "chk"
# join LOOK+Node, then join PHP
join -t',' <(echo "$LOOK_OUT"|sort) <(echo "$NODE_OUT"|sort) \
  | join -t',' - <(echo "$PHP_OUT"|sort) | awk -F',' '
  {
    name=$1; lchk=$2; lms=$3+0; nchk=$4; nms=$5+0; pchk=$6; pms=$7+0;
    rn = (nms>0)? lms/nms : 0;
    rp = (pms>0)? lms/pms : 0;
    chkok = (lchk==nchk && lchk==pchk)? "OK" : "MISMATCH";
    printf "%-22s %12.3f %12.3f %12.3f %9.2fx %9.2fx %6s\n", name, lms, nms, pms, rn, rp, chkok;
    if (lchk!=nchk || lchk!=pchk) mism++;
  }
  END { if (mism>0) printf "\n*** %d CHECKSUM MISMATCH(es) — benchmarks NOT equivalent, timings invalid for those rows ***\n", mism }'

echo
echo "=== startup / cold start (best-of-5, wall-time of a trivial script) ==="
mkdir -p "$ROOT/cpp/bench/micro/look" "$ROOT/cpp/bench/micro/node" "$ROOT/cpp/bench/micro/php"
echo 'print("x")' > "$ROOT/cpp/bench/micro/look/_hello.lk"
echo 'console.log("x")' > "$ROOT/cpp/bench/micro/node/_hello.js"
echo '<?php echo "x";' > "$ROOT/cpp/bench/micro/php/_hello.php"
cold() { # image cmd...
  local best=999999
  for i in 1 2 3 4 5; do
    local t0=$(date +%s%N)
    eval "$1" >/dev/null 2>&1
    local t1=$(date +%s%N); local ms=$(( (t1-t0)/1000000 ))
    [ $ms -lt $best ] && best=$ms
  done
  echo "${best}ms"
}
echo "  (includes docker+process spawn; relative gap is what matters)"
echo "  LOOK: $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp:/look/cpp -w /look/cpp/bench/micro/look look-build /look/cpp/build/lk _hello.lk")"
echo "  Node: $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:22-alpine node _hello.js")"
echo "  PHP:  $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/php:/app -w /app php:8.3-cli php _hello.php")"
