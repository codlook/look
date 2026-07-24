#!/usr/bin/env bash
# test_runner_test.sh — `lk test` çerçevesinin KENDİ doğruluğunu doğrular.
#
# 50. bug: assert_throws()/assert::throws() eskiden bir marker array döndürüp
# runner'ın testin DÖNÜŞ değerinde bulmasını beklerdi. Ama LOOK son ifadeyi
# örtük DÖNDÜRMEZ → marker daima atılırdı → assert_throws() hiçbir zaman assert
# ETMEZDİ. Belgelenen kullanım (assert_throws son ifade) dahil, yakalanması
# gereken hata sessizce kaçar, test YALANCI YEŞİL verirdi.
#
# Bu guard, "fırlatMASI gereken ama fırlatMAYAN" fonksiyonların assert_throws
# tarafından yakalandığını (test BAŞARISIZ olduğunu) doğrular. Eski bozuk kodda
# bu testler YEŞİL geçerdi → guard bu regresyonu yakalar (pozitif kontrol).
set -u
LK="${1:-./build/lk}"
# mutlak yola cevir (asagida test dizinine cd ediyoruz)
case "$LK" in /*) ;; *) LK="$(pwd)/$LK" ;; esac
fail=0
work="$(mktemp -d)"; mkdir -p "$work/tests"
trap 'rm -rf "$work"' EXIT

# Beklenti: assert_throws, fn firlatMAZSA testi BASARISIZ yapmali.
cat > "$work/tests/t.lk" <<'EOF'
test("son ifade, fn firlatmiyor", function() {
    assert_throws(function() { $x = 1 })
})
test("son DEGIL, fn firlatmiyor", function() {
    assert_throws(function() { $x = 1 })
    $y = 2
})
use assert;
test("modul formu, fn firlatmiyor", function() {
    assert::throws(function() { $x = 1 })
})
test("dogru kullanim, fn firlatiyor => GECMELI", function() {
    assert_throws(function() { throw "beklenen" })
    $z = 3
})
EOF

out="$(cd "$work" && "$LK" test 2>&1 || true)"
# Renk kodlarini soy
plain="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')"

# 3 basarisiz + 1/4 gecti beklenir
if ! printf '%s' "$plain" | grep -q "3 başarısız"; then
    echo "FAIL: assert_throws firlatMAYAN fn'i yakalamiyor (3 basarisiz bekleniyordu)"
    printf '%s\n' "$plain" | grep -E "❌|✅|geçti|başarısız"
    fail=1
fi
if ! printf '%s' "$plain" | grep -q "1/4 geçti"; then
    echo "FAIL: dogru assert_throws kullanimi (fn firlatiyor) GECMEDI (1/4 bekleniyordu)"
    printf '%s\n' "$plain" | grep -E "❌|✅|geçti|başarısız"
    fail=1
fi

if [ $fail = 0 ]; then
    echo "PASS: assert_throws/assert::throws — konumdan bagimsiz, HEMEN calisip firlatmayi dogruluyor (50. bug)"
fi
exit $fail
