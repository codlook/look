#!/usr/bin/env bash
# LOOK parser fail-loud guard — parser adversarial turu (2026-09-21).
# İki fail-SILENT footgun fail-LOUD yapıldı (felsefe: "hiçbir şey gizli değil"):
#   #C  kapatılmamış `/*` → EOF'a kadar sessizce yutulup kodun yarısını kaybettiriyordu
#       (exit 0, hata yok) → artık "Unterminated block comment" hatası.
#   #2  int64'ü aşan tamsayı literali → sessizce float'a düşüp presizyon kaybettiriyordu
#       → önce stderr WARN'du; 2026-11-08'den beri DERLEME HATASI (Go: "constant overflows int64";
#       tamsayı taşması kuralıyla aynı — tests/numeric_edges_test.sh).
# Pozitif+negatif kontrollü: meşru davranış korunmalı (kapalı yorum çalışır, in-range uyarmaz).
#   Kullanım: bash tests/parser_faillooud_guard.sh ./build/lk
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0
chk() { # tag, condition(0=pass)
  if [ "$2" -eq 0 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fails=$((fails+1)); fi
}

# #C: kapatılmamış /* → hata (exit!=0 + mesaj)
printf 'print(1)\n/* never closed\nprint(2)' > "$TMP/c.lk"
out=$("$LK" "$TMP/c.lk" 2>&1); rc=$?
echo "$out" | grep -qi "Unterminated block comment"; m=$?
[ $rc -ne 0 ] && [ $m -eq 0 ]; chk "#C unterminated /* -> hata" $?

# #C negatif: kapalı /* */ hâlâ çalışır (meşru davranış korunur)
printf '/* h */ print(42) /* m */\nprint(7)' > "$TMP/c2.lk"
out=$("$LK" "$TMP/c2.lk" 2>/dev/null); rc=$?
[ $rc -eq 0 ] && [ "$(echo "$out" | tr -d '[:space:]')" = "427" ]; chk "#C kapali /* */ calisir (42,7)" $?

# #2: int64-asan literal -> derleme hatasi (calismaz, cikti yok)
echo 'print(999999999999999999999999999999999999)' > "$TMP/n.lk"
out=$("$LK" "$TMP/n.lk" 2>&1); rc=$?
[ $rc -ne 0 ]; chk "#2 int64-asan literal -> hata (exit!=0)" $?
echo "$out" | grep -qiE "does not fit in 64 bits"; chk "#2 mesaj literali ve siniri soyler" $?

# #2 negatif: in-range int UYARMAZ (9007199254740993 = 2^53+1, int64 icinde)
echo 'print(9007199254740993)' > "$TMP/n2.lk"
se2=$("$LK" "$TMP/n2.lk" 2>&1 1>/dev/null)
echo "$se2" | grep -qi "exceeds"; [ $? -ne 0 ]; chk "#2 in-range int UYARMAZ" $?

echo ""
if [ $fails -ne 0 ]; then echo "PARSER FAIL-LOUD GUARD: $fails FAIL"; exit 1; fi
echo "PARSER FAIL-LOUD GUARD: TÜM VAKALAR GEÇTİ (#C hata + #2 warn, meşru davranış korundu)"
