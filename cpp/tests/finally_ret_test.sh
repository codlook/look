#!/usr/bin/env bash
# finally-on-return guard — VM + interpreter DENGE + doğru sıra. BUG: finally return'de atlanıyordu
# (VM'de try-return, interpreter'da catch-return) → sessiz kaynak-sızıntısı + differential ayrışma.
set -u
LK="${1:-./build/lk}"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/finally_ret.lk"
fail=0

vm=$("$LK" "$APP" 2>&1)
it=$(LOOK_CLI_VM=0 "$LK" "$APP" 2>&1)

# 1. İKİ MOTOR AYNI (parity) — bug tam da buradaydı
if [ "$vm" != "$it" ]; then
  echo "  FAIL: VM ≠ interpreter (finally parity bozuk)"
  echo "  --- VM ---"; echo "$vm" | sed 's/^/    /'
  echo "  --- interp ---"; echo "$it" | sed 's/^/    /'
  fail=1
fi

# 2. Beklenen davranış (VM çıktısı üzerinden — parity varsa interp de aynı)
chk() { echo "$vm" | grep -qF "$1" || { echo "  FAIL: beklenen satır yok: [$1]"; fail=1; }; }
chk "T-FIN"         # return-in-try finally çalıştı
chk "1:42"          # dönüş değeri korundu
chk "N-IC"          # nested iç finally
chk "N-DIS"         # nested dış finally
chk "C-FIN"         # return-in-catch finally çalıştı
chk "3:C"
chk "NORMAL-FIN"    # normal yol finally
chk "THROW-FIN"     # caught-throw finally
chk "B-FIN 1"       # break-out-of-try finally çalıştı
chk "K-FIN 2"       # continue-out-of-try finally çalıştı
chk "U-FIN"         # uncaught-throw finally çalıştı
chk "U-CATCH:boom"  # bug #2 fix: finally-catch'siz throw PROPAGATE eder (yutulmaz) → dışta yakalanır
chk "FIT:FINT"      # finally İÇİNDE throw → orijinali (ORIG) değiştirir (FINT dışta yakalanır)

# 3. finally BİR kez (çift-çalışma yok) — normal yolda NORMAL-FIN tam 1 kere
n=$(echo "$vm" | grep -cF "NORMAL-FIN")
[ "$n" -eq 1 ] || { echo "  FAIL: NORMAL-FIN $n kez (1 olmalı — finally çift-çalışıyor)"; fail=1; }

# bug #2 açık pozitif-kontrol: catch'siz try-finally exception'ı YUTMAMALI. Ayrı program:
# "Z" (finally) yazılmalı AMA "YUTULDU" (sonraki satır) YAZILMAMALI + exit≠0 (propagate).
tmp2="$(mktemp)"
printf 'try { throw "err" } finally { print("Z") }\nprint("YUTULDU")\n' > "$tmp2"
for eng in "" "LOOK_CLI_VM=0"; do
  out=$(env $eng "$LK" "$tmp2" 2>&1); rc=$?
  echo "$out" | grep -qF "Z" || { echo "  FAIL: [$eng] finally (Z) çalışmadı"; fail=1; }
  echo "$out" | grep -qF "YUTULDU" && { echo "  FAIL: [$eng] bug#2 — catch'siz finally exception'ı YUTTU"; fail=1; }
  [ "$rc" -ne 0 ] || { echo "  FAIL: [$eng] bug#2 — exception propagate etmedi (exit 0)"; fail=1; }
done
rm -f "$tmp2"

[ $fail = 0 ] && echo "PASS: finally (return/break/continue/catch-yut) — VM+interp parity" || echo "FAIL: finally"
exit $fail
