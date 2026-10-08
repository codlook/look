#!/usr/bin/env bash
# The command line says which engine runs a script, and its switches mean what they mean
# for the web server.
#   * LOOK_BYTECODE=0 selects the tree-walk engine for `lk` too (it was read only by the web
#     server; `lk` ignored it and ran the VM)
#   * LOOK_CLI_VM=0 selects the tree-walk engine (the switch the engine-comparison tests use):
#     shown here with a script the VM cannot compile — it runs, also with LOOK_VM_STRICT=1
#   * when the VM cannot run a script and the tree-walk engine runs it instead, `lk` says so
#     on stderr (it fell back without a word; the web server already announced it)
#   * a script the VM can run produces no such line
#   * a line that starts with ++ or -- is a new statement, not the continuation of the
#     line before
# Usage: cli_engine_choice_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
{ echo 'function cnt(...$a) { return count($a) }'; echo "print(\"got=\" . cnt($(seq -s, 1 300)))"; } > "$TMP/big.lk"
echo 'print("ok")' > "$TMP/small.lk"
printf '$x = 5\n$x--\n--$x\n$y = 1\n++$y\n$z = $x\n- 1\nprint($x . ":" . $y . ":" . $z)\n' > "$TMP/step.lk"
for sw in LOOK_CLI_VM=0 LOOK_BYTECODE=0; do
    out="$(env $sw LOOK_VM_STRICT=1 "$LK" "$TMP/big.lk" 2>"$TMP/err.txt")"; rc=$?
    if [ "$out" = "got=300" ] && [ "$rc" = "0" ] && ! grep -q 'cannot run' "$TMP/err.txt"; then echo "  OK   $sw runs the script on the tree-walk engine (a script the VM cannot compile runs, strict mode included)"
    else echo "  FAIL $sw: out=[$out] exit=$rc stderr=[$(head -c 160 "$TMP/err.txt")]"; fail=1; fi
done
out="$("$LK" "$TMP/big.lk" 2>"$TMP/err.txt")"; rc=$?
if [ "$out" = "got=300" ] && [ "$rc" = "0" ] && [ "$(grep -c 'The VM cannot run this script (compile error: .*more than 255 arguments.*tree-walk interpreter' "$TMP/err.txt")" = "1" ]; then echo "  OK   default run: the fallback is announced with its reason, and the result is right"
else echo "  FAIL default run: out=[$out] exit=$rc stderr=[$(head -c 200 "$TMP/err.txt")]"; fail=1; fi
out="$(LOOK_VM_STRICT=1 "$LK" "$TMP/big.lk" 2>&1)"; rc=$?
if [ "$rc" = "1" ]; then echo "  OK   LOOK_VM_STRICT=1 refuses instead of falling back"; else echo "  FAIL strict: exit=$rc [$out]"; fail=1; fi
out="$("$LK" "$TMP/small.lk" 2>"$TMP/err.txt")"
if [ "$out" = "ok" ] && [ ! -s "$TMP/err.txt" ]; then echo "  OK   a script the VM runs prints nothing on stderr"
else echo "  FAIL small script: out=[$out] stderr=[$(head -c 160 "$TMP/err.txt")]"; fail=1; fi
for sw in LOOK_VM_STRICT=1 LOOK_CLI_VM=0; do
    out="$(env $sw "$LK" "$TMP/step.lk" 2>&1 | tail -1)"
    if [ "$out" = "3:2:2" ]; then echo "  OK   [$sw] lines starting with ++ / -- are statements; a line starting with - still continues"
    else echo "  FAIL [$sw] step.lk: [$out]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: the command line says which engine runs" || { echo "FAIL: command-line engine choice"; exit 1; }
