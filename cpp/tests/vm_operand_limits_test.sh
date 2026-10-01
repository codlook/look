#!/usr/bin/env bash
# VM operand-width guard. Argument count and parameter index travel in 8 bits; past 255
# they used to WRAP silently on the VM (a call with 300 arguments delivered 44, a
# function with 300 parameters returned the 44th), while tree-walk was correct.
# Now the compiler refuses: the default run falls back to the interpreter (loudly) and
# gives the right answer, and LOOK_VM_STRICT=1 reports the limit by name.
# Usage: vm_operand_limits_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
nums() { local s=""; for i in $(seq 1 $(( $1 - 1 ))); do s="$s$2$i, "; done; printf '%s%s%s' "$s" "$2" "$1"; }

{ echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(nums 300 '')))"; } > "$TMP/args.lk"
{ echo "function big($(nums 300 '$p')) { return \$p300 }"; echo "print(\"got=\" . big($(nums 300 '')))"; } > "$TMP/params.lk"
# Control: exactly at the limit must still compile and run on the VM.
{ echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(nums 200 '')))"; } > "$TMP/ok.lk"

check() {  # name file want-default want-strict-substring
    local d s
    d="$("$LK" "$2" 2>/dev/null | tail -1)"
    s="$(LOOK_VM_STRICT=1 "$LK" "$2" 2>&1 | tail -1)"
    if [ "$d" = "$3" ]; then echo "  OK   $1: default run -> $d"; else echo "  FAIL $1: default run got [$d] want [$3]"; fail=1; fi
    case "$s" in *"$4"*) echo "  OK   $1: strict VM -> ${4}";; *) echo "  FAIL $1: strict VM got [$s] want [...$4...]"; fail=1;; esac
}
check "300 arguments"  "$TMP/args.lk"   "got=300" "more than 255 arguments"
check "300 parameters" "$TMP/params.lk" "got=300" "more than 255 parameters"
check "200 arguments"  "$TMP/ok.lk"     "got=200" "got=200"
[ $fail = 0 ] && echo "PASS: vm operand limits" || { echo "FAIL: vm operand limits"; exit 1; }
