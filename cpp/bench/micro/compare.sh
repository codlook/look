#!/usr/bin/env bash
# Sequential, isolated timing of the LOOK / Node / PHP micro-benchmark suites + cross-language
# checksum equivalence guard. One process at a time (no contention); every suite runs RUNS times
# (default 5) and the MEDIAN warm_ms of each benchmark is reported.
# LK_BIN=<dir with lk> chooses the LOOK binary (default: cpp/build).
# Fairness lock (see BENCHMARK.md): Node 22 default (JIT on), PHP 8.3 with OPcache + JIT on.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # repo root
RUNS="${RUNS:-5}"
LK_BIN="${LK_BIN:-$ROOT/cpp/build}"
LK="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp:/look/cpp -v $LK_BIN:/lkbin:ro -w /look/cpp/bench/micro/look look-build /lkbin/lk run.lk"
ND="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:22-alpine node run.js"
PHP_JIT="-d zend_extension=opcache.so -d opcache.enable_cli=1 -d opcache.jit_buffer_size=128M -d opcache.jit=tracing"
PH="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/php:/app -w /app php:8.3-cli php $PHP_JIT run.php"

run3() {  # command... -> prints "name,checksum,median_ms" per benchmark (median over RUNS runs)
  local all="" i
  for i in $(seq 1 "$RUNS"); do all="$all$(eval "$1" 2>/dev/null)"$'\n'; done
  printf '%s' "$all" | awk -F',' 'NF>=3 { n[$1]++; v[$1, n[$1]] = $3 + 0; c[$1] = $2 }
    END {
      for (k in n) {
        m = n[k]
        for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++)
          if (v[k, j] < v[k, i]) { t = v[k, i]; v[k, i] = v[k, j]; v[k, j] = t }
        printf "%s,%s,%.3f\n", k, c[k], v[k, int((m + 1) / 2)]
      }
    }'
}

echo "=== running LOOK suite (median of $RUNS, isolated) ==="
LOOK_OUT=$(run3 "$LK"); echo "$LOOK_OUT"
echo "=== running Node suite (Node 22, median of $RUNS, isolated) ==="
NODE_OUT=$(run3 "$ND"); echo "$NODE_OUT"
echo "=== running PHP suite (8.3 + OPcache + JIT, median of $RUNS, isolated) ==="
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
echo "  LOOK: $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp:/look/cpp -v $LK_BIN:/lkbin:ro -w /look/cpp/bench/micro/look look-build /lkbin/lk _hello.lk")"
echo "  Node: $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:22-alpine node _hello.js")"
echo "  PHP:  $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/php:/app -w /app php:8.3-cli php _hello.php")"
