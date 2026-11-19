#!/usr/bin/env bash
# finally-on-return guard — correct order + expected lines. BUG: finally was skipped on return
# (try-return in the VM, catch-return in the old interpreter) → silent resource leak.
set -u
LK="${1:-./build/lk}"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/finally_ret.lk"
fail=0

vm=$("$LK" "$APP" 2>&1)

# Expected behavior
chk() { echo "$vm" | grep -qF "$1" || { echo "  FAIL: expected line missing: [$1]"; fail=1; }; }
chk "T-FIN"         # return-in-try finally ran
chk "1:42"          # return value preserved
chk "N-IC"          # nested inner finally
chk "N-DIS"         # nested outer finally
chk "C-FIN"         # return-in-catch finally ran
chk "3:C"
chk "NORMAL-FIN"    # normal path finally
chk "THROW-FIN"     # caught-throw finally
chk "B-FIN 1"       # break-out-of-try finally ran
chk "K-FIN 2"       # continue-out-of-try finally ran
chk "U-FIN"         # uncaught-throw finally ran
chk "U-CATCH:boom"  # bug #2 fix: a throw without catch PROPAGATES through finally (not swallowed) → caught outside
chk "FIT:FINT"      # throw INSIDE finally → replaces the original (ORIG) (FINT is caught outside)

# finally ONCE (no double run) — on the normal path NORMAL-FIN exactly 1 time
n=$(echo "$vm" | grep -cF "NORMAL-FIN")
[ "$n" -eq 1 ] || { echo "  FAIL: NORMAL-FIN ran $n times (should be 1 — finally runs twice)"; fail=1; }

# explicit positive control for bug #2: try-finally without catch must NOT SWALLOW the exception. Separate program:
# "Z" (finally) must be printed BUT "YUTULDU" (next line) must NOT BE PRINTED + exit≠0 (propagate).
tmp2="$(mktemp)"
printf 'try { throw "err" } finally { print("Z") }\nprint("YUTULDU")\n' > "$tmp2"
out=$("$LK" "$tmp2" 2>&1); rc=$?
echo "$out" | grep -qF "Z" || { echo "  FAIL: finally (Z) did not run"; fail=1; }
echo "$out" | grep -qF "YUTULDU" && { echo "  FAIL: bug#2 — finally without catch SWALLOWED the exception"; fail=1; }
[ "$rc" -ne 0 ] || { echo "  FAIL: bug#2 — the exception did not propagate (exit 0)"; fail=1; }
rm -f "$tmp2"

[ $fail = 0 ] && echo "PASS: finally (return/break/continue/catch-swallow) — expected lines" || echo "FAIL: finally"
exit $fail
