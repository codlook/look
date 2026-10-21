#!/usr/bin/env bash
# VM operand-width guard.
#
# Operands travel in 16 bits and two of them form a 32-bit field (constant index, jump
# target). Up to LOOK 1 they were 8 bits: past 255 arguments / parameters / captures /
# registers (and 65535 constants, or a loop body longer than 65535 instructions) a program
# did not compile on the VM, and LOOK 2 refuses to start an app the VM cannot compile. Each
# case here runs on the VM and must give the right answer. Above the real
# limit (65534) the compiler still refuses, and names the limit (no silent wrap-around: in
# v1 a call with 300 arguments once delivered 44).
# Usage: vm_operand_limits_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
nums() { local s=""; for i in $(seq 1 $(( $1 - 1 ))); do s="$s$2$i, "; done; printf '%s%s%s' "$s" "$2" "$1"; }

{ echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(nums 300 '')))"; } > "$TMP/args.lk"
{ echo "function big($(nums 300 '$p')) { return \$p300 }"; echo "print(\"got=\" . big($(nums 300 '')))"; } > "$TMP/params.lk"
{ echo 'function f() {'; for i in $(seq 1 300); do echo "  \$v$i = $i"; done; echo '  return "got=" . ($v1 + $v300)'; echo '}'; echo 'print(f())'; } > "$TMP/locals.lk"
{ echo 'function outer() {'; for i in $(seq 1 300); do echo "  \$c$i = 1"; done
  printf '  $g = fn() => $c1'; for i in $(seq 2 300); do printf ' + $c%d' $i; done; echo
  echo '  return "got=" . $g()'; echo '}'; echo 'print(outer())'; } > "$TMP/captures.lk"
{ echo 'use string'; echo '$t = 0'; for i in $(seq 1 70000); do echo "\$t = \$t + string::len(\"s$i\")"; done; echo 'print("got=" . $t)'; } > "$TMP/consts.lk"
{ echo '$t = 0'; echo 'for ($r = 0; $r < 2; $r = $r + 1) {'; for i in $(seq 1 40000); do echo "  \$t = \$t + 1"; done; echo '}'; echo 'print("got=" . $t)'; } > "$TMP/longloop.lk"
{ echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(nums 65535 '')))"; } > "$TMP/over.lk"

check() {  # name file want
    local s
    s="$("$LK" "$2" 2>&1 | tail -1)"
    if [ "$s" = "$3" ]; then echo "  OK   $1 on the VM -> $s"; else echo "  FAIL $1 on the VM: got [$s] want [$3]"; fail=1; fi
}
check "300 arguments"           "$TMP/args.lk"     "got=300"
check "300 parameters"          "$TMP/params.lk"   "got=300"
check "300 local variables"     "$TMP/locals.lk"   "got=301"
check "300 captured variables"  "$TMP/captures.lk" "got=300"
check "70000 constants"         "$TMP/consts.lk"   "got=408894"
check "loop body over 65535 instructions" "$TMP/longloop.lk" "got=80000"
s="$("$LK" "$TMP/over.lk" 2>&1 | grep -m1 "arguments")"
case "$s" in *"more than 65534 arguments"*) echo "  OK   65535 arguments: refused by name";;
             *) echo "  FAIL 65535 arguments: got [$s] (want the 65534 limit named)"; fail=1;; esac
[ $fail = 0 ] && echo "PASS: vm operand limits" || { echo "FAIL: vm operand limits"; exit 1; }
