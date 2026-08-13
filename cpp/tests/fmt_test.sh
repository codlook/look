#!/usr/bin/env bash
# lk fmt guard'ı — idempotency + ANLAM-KORUMA + --check exit kodları + yorum/string koruma.
# fmt yalnız girinti/boşluk değiştirir; token dizisi değişirse fmt İPTAL eder → anlam korunur.
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

# 1. --check biçimsiz dosyada exit 1
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 1 ] || { echo "  FAIL: --check biçimsizde exit 1 vermedi"; fail=1; }
# 2. biçimle
"$LK" fmt "$TMP/m.lk" >/dev/null 2>&1
# 3. --check artık exit 0 (idempotent)
"$LK" fmt --check "$TMP/m.lk" >/dev/null 2>&1; [ $? -eq 0 ] || { echo "  FAIL: biçimlenmiş dosya --check exit 0 vermedi"; fail=1; }
# 4. ANLAM-KORUMA: biçimlenmiş dosya parse ediliyor
"$LK" --check "$TMP/m.lk" >/dev/null 2>&1 || { echo "  FAIL: biçimlenmiş dosya parse edilemiyor (anlam bozuldu)"; fail=1; }
grep -q '^        return' "$TMP/m.lk" || { echo "  FAIL: girinti uygulanmadı (return 2-derinlikte olmalı)"; fail=1; }
grep -q '^# ust yorum' "$TMP/m.lk" || { echo "  FAIL: üst yorum korunmadı"; fail=1; }
grep -q '# ic yorum'    "$TMP/m.lk" || { echo "  FAIL: satır-içi yorum korunmadı"; fail=1; }
# 5. idempotency: ikinci fmt hiçbir şey değiştirmemeli
cp "$TMP/m.lk" "$TMP/m2.lk"; "$LK" fmt "$TMP/m2.lk" >/dev/null 2>&1
diff -q "$TMP/m.lk" "$TMP/m2.lk" >/dev/null || { echo "  FAIL: idempotent değil (fmt(fmt(x)) != fmt(x))"; fail=1; }

# 6. POZİTİF-KONTROL: çok-satırlı string içi ve içindeki '{' korunmalı.
#    fmt string içini yeniden-girintilerse anlam-koruma yakalar → dosya değişmez/parse eder.
printf '$s = "a\n  b { c }"\n$x = 1 + 2\n' > "$TMP/str.lk"
cp "$TMP/str.lk" "$TMP/str_orig.lk"
"$LK" fmt "$TMP/str.lk" >/dev/null 2>&1
"$LK" --check "$TMP/str.lk" >/dev/null 2>&1 || { echo "  FAIL: çok-satırlı string bozuldu (parse edilemiyor)"; fail=1; }
# string içi 2. satır ('  b { c }') AYNEN kalmalı (girinti string verisi)
grep -q '^  b { c }' "$TMP/str.lk" || { echo "  FAIL: string-içi satır yeniden-girintilendi (veri bozuldu)"; fail=1; }

[ $fail = 0 ] && echo "PASS: lk fmt (idempotent + anlam-koruma + yorum/string)" || echo "FAIL: lk fmt"
exit $fail
