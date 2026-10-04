#!/usr/bin/env bash
# Baseline for the value-semantics work: wall time of five array workloads, both engines,
# best of 3. Usage: run.sh <lk>   (run from a container-local copy, not a bind mount)
LK="${1:?usage: $0 <lk>}"; cd "$(dirname "$0")"
printf '%-20s %12s %12s   %s\n' workload "VM ms" "tree-walk ms" output
for w in local_write global_write nested_write pass_read assign_then_write; do
  row=""; out=""
  for env in LOOK_VM_STRICT=1 LOOK_CLI_VM=0; do best=999999
    for i in 1 2 3; do t0=$(date +%s%N); out="$(env $env "$LK" $w.lk 2>&1 | tail -1)"; t1=$(date +%s%N); ms=$(( (t1 - t0) / 1000000 )); [ $ms -lt $best ] && best=$ms; done
    row="$row $(printf '%12s' $best)"; done
  printf '%-20s%s   %s\n' $w "$row" "$(echo "$out" | cut -c1-40)"
done
