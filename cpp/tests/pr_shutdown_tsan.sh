#!/usr/bin/env bash
# TSan positive control for the parallel_runtime shutdown race.
#   BUG (-DLOOK_PR_STATIC_TEST): the old file-static → an s_cv destructor race is expected.
#   FIX (default): the primitives leak → no destructor → no race.
# Note: in a container TSan sometimes gives "FATAL: unexpected memory mapping" (ASLR
# noise, NOT a race) → counted separately. Only "data race" is a real finding.
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null

CF="-std=c++23 -O1 -g -fsanitize=thread -I include src/parallel_runtime.cpp tests/pr_shutdown_tsan.cpp -pthread"
g++ $CF -DLOOK_PR_STATIC_TEST -o /tmp/pr_bug
g++ $CF               -o /tmp/pr_fix

tally() {
  local bin="$1"; local race=0 fatal=0 clean=0 i o
  for i in $(seq 1 30); do
    o=$("$bin" 2>&1)
    if echo "$o" | grep -q "data race"; then race=$((race+1))
    elif echo "$o" | grep -q "FATAL"; then fatal=$((fatal+1))
    else clean=$((clean+1)); fi
  done
  echo "race=$race clean=$clean fatal=$fatal (30 kosum)"
}

{
  echo "BUG (statik): $(tally /tmp/pr_bug)"
  echo "FIX (leak)  : $(tally /tmp/pr_fix)"
  echo "--- BUG ornek rapor (ilk yaris) ---"
  for i in $(seq 1 20); do o=$(/tmp/pr_bug 2>&1); echo "$o" | grep -q "data race" && { echo "$o" | grep -iE "data race|cond_destroy|look::s_" | head -3; break; }; done
} > tests/_tsan_res.txt 2>&1
echo DONE
