#!/usr/bin/env bash
# LOOK parser fail-loud guard — parser adversarial round (2026-09-21).
# Two fail-SILENT footguns were made fail-LOUD (philosophy: "nothing is hidden"):
#   #C  an unclosed `/*` was silently swallowed up to EOF and made half of the code disappear
#       (exit 0, no error) → now an "Unterminated block comment" error.
#   #2  an integer literal beyond int64 silently dropped to float and lost precision
#       → first a stderr WARN; since 2026-11-08 a COMPILE ERROR (Go: "constant overflows int64";
#       the same as the integer overflow rule — tests/numeric_edges_test.sh).
# With positive+negative controls: legitimate behavior must be preserved (a closed comment works, in-range does not warn).
#   Usage: bash tests/parser_faillooud_guard.sh ./build/lk
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0
chk() { # tag, condition(0=pass)
  if [ "$2" -eq 0 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fails=$((fails+1)); fi
}

# #C: unclosed /* → error (exit!=0 + message)
printf 'print(1)\n/* never closed\nprint(2)' > "$TMP/c.lk"
out=$("$LK" "$TMP/c.lk" 2>&1); rc=$?
echo "$out" | grep -qi "Unterminated block comment"; m=$?
[ $rc -ne 0 ] && [ $m -eq 0 ]; chk "#C unterminated /* -> error" $?

# #C negative: a closed /* */ still works (legitimate behavior preserved)
printf '/* h */ print(42) /* m */\nprint(7)' > "$TMP/c2.lk"
out=$("$LK" "$TMP/c2.lk" 2>/dev/null); rc=$?
[ $rc -eq 0 ] && [ "$(echo "$out" | tr -d '[:space:]')" = "427" ]; chk "#C closed /* */ works (42,7)" $?

# #2: int64-exceeding literal -> compile error (does not run, no output)
echo 'print(999999999999999999999999999999999999)' > "$TMP/n.lk"
out=$("$LK" "$TMP/n.lk" 2>&1); rc=$?
[ $rc -ne 0 ]; chk "#2 int64-overflowing literal -> error (exit!=0)" $?
echo "$out" | grep -qiE "does not fit in 64 bits"; chk "#2 the message names the literal and the limit" $?

# #2 negative: an in-range int does NOT warn (9007199254740993 = 2^53+1, within int64)
echo 'print(9007199254740993)' > "$TMP/n2.lk"
se2=$("$LK" "$TMP/n2.lk" 2>&1 1>/dev/null)
echo "$se2" | grep -qi "exceeds"; [ $? -ne 0 ]; chk "#2 an in-range int does NOT warn" $?

echo ""
if [ $fails -ne 0 ]; then echo "PARSER FAIL-LOUD GUARD: $fails FAIL"; exit 1; fi
echo "PARSER FAIL-LOUD GUARD: ALL CASES PASSED (#C error + #2 warn, legitimate behavior preserved)"
