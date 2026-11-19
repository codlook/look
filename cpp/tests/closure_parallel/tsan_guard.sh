#!/usr/bin/env bash
# closure_parallel/tsan_guard.sh — guard for the parallel closure-capture deep-clone.
#
# FINDING (58 capture+parallel axis): source analysis — when a parallel task captures a CLOSURE,
# the PARALLEL_CALL deep-clone copied that closure SHALLOWLY (deep_clone_impl returns `*this`
# for a fn) → the cell inside the closure stayed MECHANICALLY SHARED with the parent.
# FIX: deep-clone is transitive — closure captures are cloned too (bc_fn_cloner hook +
# PARALLEL_CALL closure clone). Defensive hardening consistent with the isolation intent of 2b.
#
# LEVEL OF EVIDENCE (HONEST): This is a MECHANICAL/DEFENSIVE fix — an OBSERVABLE RACE could not be
# PRODUCED independently. 15+ runs and many scenarios (read-write, write-write,
# with/without channel) were tried; even the BUG variant (LOOK_NO_TRANSITIVE_CLONE, md5-verified)
# came out TSan-clean. The earlier "np1 cell race" attribution was probably a misattribution of the
# s_cv shutdown race (which is also in the heap, on every parallel+exit) — WITHDRAWN. The source
# shows the shallow copy (isolation missing) but in practice no race could be triggered. LESSON: in a TSan
# "heap race" attribution, the allocation stack must DISTINGUISH s_cv from the cell.
#
# POSITIVE CONTROL: -DLOOK_NO_TRANSITIVE_CLONE builds the old (shallow) state. With the FIX np1 is functionally
# isolated (0), np2 is isolated (123). No observable difference could be produced in the BUG (see above).
#
# Usage (Docker, gcc-toolset-12 + libtsan):
#   dnf install -y gcc-toolset-12-libtsan-devel; source /opt/rh/gcc-toolset-12/enable
#   echo 0 > /proc/sys/kernel/randomize_va_space   # for the TSan ASLR FATAL noise (privileged)
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOOK_SANITIZE=thread \
#         -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so
#   cmake --build build-tsan --target look -j8
#   bash cpp/tests/closure_parallel/tsan_guard.sh build-tsan/lk
set -u
LK="${1:-build-tsan/lk}"
DIR="$(dirname "$0")"
fail=0
for i in 1 2 3 4 5 6 7 8; do
  o=$(TSAN_OPTIONS="halt_on_error=0" "$LK" "$DIR/np1.lk" 2>&1)
  if echo "$o" | grep -q "data race"; then
    echo "FAIL np1: RACE on a cell inside a closure (transitive deep-clone missing)"; echo "$o" | grep -iE "data race|heap block" | head -2
    fail=1; break
  fi
done
[ $fail = 0 ] && echo "PASS np1: closure-capture deep-clone is transitive — no race"
# functional isolation (VM): np1=0 (isolated), np2=123 (isolated)
v1=$("$LK" "$DIR/np1.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
v2=$("$LK" "$DIR/np2.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
[ "$v1" = "0" ] && echo "PASS np1 isolated: 0" || { echo "FAIL np1 isolated: [$v1] (expected 0)"; fail=1; }
[ "$v2" = "123" ] && echo "PASS np2 isolated: 123" || { echo "FAIL np2 isolated: [$v2] (expected 123)"; fail=1; }
exit $fail
