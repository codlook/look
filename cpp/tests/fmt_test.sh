#!/usr/bin/env bash
# lk fmt guard — idempotency + MEANING PRESERVATION + --check exit codes + comment/string preservation.
# fmt only changes indentation/whitespace; if the token sequence changes fmt ABORTS → meaning is preserved.
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/m.lk" <<'LK'
# ust yorum
function f($a,$b){
if($a>0){
return $a+$b   # ic yorum
}
}
LK

# 1. --check exits 1 on an unformatted file
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 1 ] || { echo "  FAIL: --check did not exit 1 on unformatted input"; fail=1; }
# 2. format
"$LK" fmt "$TMP/m.lk" >/dev/null 2>&1
# 3. --check now exits 0 (idempotent)
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 0 ] || { echo "  FAIL: formatted file did not give --check exit 0"; fail=1; }
# 4. MEANING PRESERVATION: the formatted file parses
"$LK" --check "$TMP/m.lk" >/dev/null 2>&1 || { echo "  FAIL: formatted file does not parse (meaning changed)"; fail=1; }
grep -q '^        return' "$TMP/m.lk" || { echo "  FAIL: indentation was not applied (return should be at depth 2)"; fail=1; }
grep -q '^# ust yorum' "$TMP/m.lk" || { echo "  FAIL: top comment was not preserved"; fail=1; }
grep -q '# ic yorum'    "$TMP/m.lk" || { echo "  FAIL: inline comment was not preserved"; fail=1; }
# 5. idempotency: a second fmt must change nothing
cp "$TMP/m.lk" "$TMP/m2.lk"; "$LK" fmt "$TMP/m2.lk" >/dev/null 2>&1
diff -q "$TMP/m.lk" "$TMP/m2.lk" >/dev/null || { echo "  FAIL: not idempotent (fmt(fmt(x)) != fmt(x))"; fail=1; }

# 6. POSITIVE CONTROL: the inside of a multi-line string and the '{' in it must be preserved.
#    if fmt re-indents inside a string, meaning preservation catches it → the file is unchanged/parses.
printf '$s = "a\n  b { c }"\n$x = 1 + 2\n' > "$TMP/str.lk"
cp "$TMP/str.lk" "$TMP/str_orig.lk"
"$LK" fmt "$TMP/str.lk" >/dev/null 2>&1
"$LK" --check "$TMP/str.lk" >/dev/null 2>&1 || { echo "  FAIL: multi-line string was corrupted (does not parse)"; fail=1; }
# the 2nd line inside the string ('  b { c }') must stay AS IS (indentation is string data)
grep -q '^  b { c }' "$TMP/str.lk" || { echo "  FAIL: a line inside a string was re-indented (data corrupted)"; fail=1; }

[ $fail = 0 ] && echo "PASS: lk fmt (idempotent + meaning-preserving + comment/string)" || echo "FAIL: lk fmt"
exit $fail
