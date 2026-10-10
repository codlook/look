#!/bin/bash
# VM→interpreter fallback DOĞRULUK guard'ı (olgunluk haritası "web/fcgi fallback" boşluğu).
#
# NE KORUR: lk-fcgi'de bir route VM'de hata verince sunucu SESSİZCE tree-walk
# interpreter'a düşer (doğru-ama-yavaş yanıt döner, bug'ı yalnız log'a yazar).
# Bu fallback YILLARCA BUG MASKELEDİ (2026-07-16 turu). Ama fallback'in KENDİSİNİN
# doğru davrandığını test eden guard YOKTU — çünkü VM'i kasıtlı hataya sokacak bir
# test-hook'u yoktu. Bu guard o hook'u (LOOK_VM_FORCE_FAIL) kullanır.
#
# ÜÇ SENARYO:
#   A) hook YOK           → yanıt VM hızlı yolundan, DOĞRU, log'da "VM BUG" YOK.
#   B) LOOK_VM_FORCE_FAIL → VM o route'ta fail, interpreter fallback devreye girer:
#                           yanıt HÂLÂ DOĞRU (== A) + log'da "VM BUG" fallback kaydı VAR.
#   C) B + LOOK_VM_STRICT=1 → fallback KAPALI: aynı route artık 500 döner (maskeleme yok).
#
# POZİTİF KONTROL (Kural 1): B, gerçekten fallback yolunu egzersiz etmeli — bu yüzden
# hem (yanıt==beklenen) hem (log'da "VM BUG") aranır. Değer bozulursa (fallback yanlış
# hesaplarsa) yanıt!=beklenen → FAIL. Log yoksa (fallback tetiklenmedi) → FAIL.
# A'da "VM BUG" ARANMAMALI (fast-path egzersiz edildiğinin kanıtı; yoksa test anlamsız).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7772}"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"
TMP=$(mktemp -d)
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }
fail=0

# Deterministik hesap: iki motorda BİREBİR aynı sonucu vermeli (1+..+100 = 5050).
cat > "$TMP/app.lk" <<'LKEOF'
route("GET", "/calc", function() {
    $s = 0
    for ($i = 1; $i <= 100; $i = $i + 1) { $s = $s + $i }
    return response::text("RESULT=" . $s)
})
LKEOF
EXPECT="RESULT=5050"

start_srv() {  # $1..: ekstra env atamaları
  "$@" "$FCGI" --mode http --port "$PORT" "$TMP/app.lk" >"$SRVLOG" 2>&1 &
  SRV=$!
  disown "$SRV" 2>/dev/null   # job-control "Terminated" gürültüsünü sustur
  for i in $(seq 1 40); do
    # Hazır olma: 404 route'una başarılı bağlantı (ss image'de yok — curl ile poll)
    curl -s -o /dev/null "http://127.0.0.1:$PORT/__ping" 2>/dev/null && return 0
    kill -0 "$SRV" 2>/dev/null || { echo "  FAIL: sunucu başlamadı"; cat "$SRVLOG"; return 1; }
    sleep 0.25
  done
  echo "  FAIL: sunucu $PORT'ta dinlemedi"; return 1
}
stop_srv() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 0.5; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; SRV=""; }

# ── A) hook YOK — VM hızlı yolu ─────────────────────────────────────────────
SRVLOG="$TMP/a.log"
start_srv env || { echo "FAIL: VM fallback guard"; exit 1; }
A=$(curl -s "http://127.0.0.1:$PORT/calc" 2>/dev/null)
stop_srv
if [ "$A" = "$EXPECT" ]; then echo "  OK A(VM): yanıt '$A'"
else echo "  FAIL A(VM): yanıt '$A' != '$EXPECT'"; fail=1; fi
if grep -q "VM BUG" "$SRVLOG"; then
  echo "  FAIL A: hook YOK ama log'da 'VM BUG' var — fast-path egzersiz edilmedi (test anlamsız)"; fail=1
else echo "  OK A: log'da fallback YOK (VM fast-path doğrulandı)"; fi

# ── B) LOOK_VM_FORCE_FAIL — a VM error is a 500; the request is NOT run again ──
# (1.0.8) Until 1.0.7 the request was run a second time on the tree-walk engine and answered
# from there. That second run repeated every side effect of a handler that had failed halfway
# (tests/no_double_execution_test.sh), so it is gone: an error on the VM is an error.
SRVLOG="$TMP/b.log"
start_srv env LOOK_VM_FORCE_FAIL="/calc" || { echo "FAIL: VM fallback guard"; exit 1; }
BCODE=$(curl -s -o "$TMP/b.body" -w "%{http_code}" "http://127.0.0.1:$PORT/calc" 2>/dev/null)
B=$(cat "$TMP/b.body" 2>/dev/null)
stop_srv
if [ "$BCODE" = "500" ] && [ "$B" != "$EXPECT" ]; then echo "  OK B(VM error): HTTP 500, not answered by a second run"
else echo "  FAIL B(VM error): code=$BCODE body='$B' — the request was answered by a second run"; fail=1; fi
if grep -q "Dispatch error" "$SRVLOG" && ! grep -q "fell back to the interpreter\|interpreter fallback" "$SRVLOG"; then echo "  OK B: the error is logged, no fallback"
else echo "  FAIL B: log does not show a plain dispatch error"; fail=1; fi

# ── C) LOOK_VM_STRICT=1 — the same: 500, and the server keeps answering ─────
SRVLOG="$TMP/c.log"
start_srv env LOOK_VM_FORCE_FAIL="/calc" LOOK_VM_STRICT=1 || { echo "FAIL: VM fallback guard"; exit 1; }
CODE=$(curl -s -o "$TMP/c.body" -w "%{http_code}" "http://127.0.0.1:$PORT/calc" 2>/dev/null)
for i in 1 2 3 4 5 6 7 8 9 10; do curl -s -m 3 -o /dev/null "http://127.0.0.1:$PORT/calc"; done
CODE2=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/calc" 2>/dev/null)
stop_srv
if [ "$CODE" = "500" ] && [ "$CODE2" = "500" ]; then echo "  OK C(strict): HTTP 500, still answering after 11 errors"
else echo "  FAIL C(strict): first=$CODE after-11=$CODE2 (000 = the server stopped answering)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: VM errors are errors, reported once" || echo "FAIL: VM error / interpreter route guard"
exit $fail
