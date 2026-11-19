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
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 1 ] || { echo "  FAIL: --check biçimsizde exit 1 vermedi"; fail=1; }
# 2. format
"$LK" fmt "$TMP/m.lk" >/dev/null 2>&1
# 3. --check now exits 0 (idempotent)
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 0 ] || { echo "  FAIL: biçimlenmiş dosya --check exit 0 vermedi"; fail=1; }
# 4. MEANING PRESERVATION: the formatted file parses
"$LK" --check "$TMP/m.lk" >/dev/null 2>&1 || { echo "  FAIL: biçimlenmiş dosya parse edilemiyor (anlam bozuldu)"; fail=1; }
grep -q '^        return' "$TMP/m.lk" || { echo "  FAIL: girinti uygulanmadı (return 2-derinlikte olmalı)"; fail=1; }
grep -q '^# ust yorum' "$TMP/m.lk" || { echo "  FAIL: üst yorum korunmadı"; fail=1; }
grep -q '# ic yorum'    "$TMP/m.lk" || { echo "  FAIL: satır-içi yorum korunmadı"; fail=1; }
# 5. idempotency: a second fmt must change nothing
cp "$TMP/m.lk" "$TMP/m2.lk"; "$LK" fmt "$TMP/m2.lk" >/dev/null 2>&1
diff -q "$TMP/m.lk" "$TMP/m2.lk" >/dev/null || { echo "  FAIL: idempotent değil (fmt(fmt(x)) != fmt(x))"; fail=1; }

# 6. POSITIVE CONTROL: the inside of a multi-line string and the '{' in it must be preserved.
#    if fmt re-indents inside a string, meaning preservation catches it → the file is unchanged/parses.
printf '$s = "a\n  b { c }"\n$x = 1 + 2\n' > "$TMP/str.lk"
cp "$TMP/str.lk" "$TMP/str_orig.lk"
"$LK" fmt "$TMP/str.lk" >/dev/null 2>&1
"$LK" --check "$TMP/str.lk" >/dev/null 2>&1 || { echo "  FAIL: çok-satırlı string bozuldu (parse edilemiyor)"; fail=1; }
# the 2nd line inside the string ('  b { c }') must stay AS IS (indentation is string data)
grep -q '^  b { c }' "$TMP/str.lk" || { echo "  FAIL: string-içi satır yeniden-girintilendi (veri bozuldu)"; fail=1; }

[ $fail = 0 ] && echo "PASS: lk fmt (idempotent + anlam-koruma + yorum/string)" || echo "FAIL: lk fmt"
exit $fail
