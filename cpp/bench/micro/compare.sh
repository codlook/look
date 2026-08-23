#!/usr/bin/env bash
# Sequential, isolated timing of the two micro-benchmark suites + cross-language
# checksum equivalence guard. Run ONLY after both agents have produced their suites.
# One process at a time (no contention) — best-of-3, keep the MIN warm_ms per benchmark.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # repo root
LK="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp:/look/cpp -w /look/cpp/bench/micro/look look-build /look/cpp/build/lk run.lk"
ND="MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:20-alpine node run.js"

run3() {  # command... -> prints "name,checksum,min_ms" per benchmark (min over 3 runs)
  local out1 out2 out3
  out1=$(eval "$1" 2>/dev/null); out2=$(eval "$1" 2>/dev/null); out3=$(eval "$1" 2>/dev/null)
  # merge: for each name, keep min warm_ms; checksum from run1
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
echo "=== running Node suite (best-of-3, isolated) ==="
NODE_OUT=$(run3 "$ND"); echo "$NODE_OUT"

echo
echo "=== comparison (checksum MUST match = equivalence guard) ==="
printf "%-22s %14s %14s %10s %8s\n" "benchmark" "LOOK_ms" "Node_ms" "LOOK/Node" "chk"
join -t',' <(echo "$LOOK_OUT"|sort) <(echo "$NODE_OUT"|sort) | awk -F',' '
  {
    name=$1; lchk=$2; lms=$3+0; nchk=$4; nms=$5+0;
    ratio = (nms>0)? lms/nms : 0;
    chkok = (lchk==nchk)? "OK" : "MISMATCH";
    printf "%-22s %14.3f %14.3f %10.2fx %8s\n", name, lms, nms, ratio, chkok;
    if (lchk!=nchk) mism++;
  }
  END { if (mism>0) printf "\n*** %d CHECKSUM MISMATCH(es) — benchmarks NOT equivalent, timings invalid for those rows ***\n", mism }'

echo
echo "=== startup / cold start (best-of-5, wall-time of a trivial script) ==="
mkdir -p "$ROOT/cpp/bench/micro/look" "$ROOT/cpp/bench/micro/node"
echo 'print("x")' > "$ROOT/cpp/bench/micro/look/_hello.lk"
echo 'console.log("x")' > "$ROOT/cpp/bench/micro/node/_hello.js"
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
echo "  Node: $(cold "MSYS_NO_PATHCONV=1 docker run --rm -v $ROOT/cpp/bench/micro/node:/app -w /app node:20-alpine node _hello.js")"
