#!/bin/bash
# SSE sse::close() fd-sızıntısı regresyon guard'ı (957d11e fix'inin kalıcılaştırılmış hâli).
#
# BUG (ölçülmüştü, düşmanca bug-avı 2026-08-11): sunucu sse::close() çağırınca fd/sse_clients/
# epoll kaydı, istemci TCP-kopana kadar SIZIYORDU (close_conn yalnız closed işaretliyordu, gerçek
# temizlik event-loop'un sonraki READ olayına ertelenmişti). Ayrıca soket kapatılmadığı için
# istemci akışın bittiğini bilmiyordu (asılı kalıyordu). Fix: close_conn → loop->post(close_sse)
# (loop-thread temizlik) + own-once guard (çift-close yok).
#
# POZİTİF KONTROL (Kural 1): fix ÖNCESİ bu test KIRMIZI'ydı — sse::close sonrası fd = base+1
# (sızıntı). Fix SONRASI fd = base (serbest) + curl EOF alır (soket gerçekten kapanır).
# Dedektör = /proc/PID/fd sayacı (fd-yaşam yarışı TSan-kör olduğundan doğru araç budur).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7768}"
TMP=$(mktemp -d)
SRV=""; CURL=""
cleanup(){ [ -n "$CURL" ] && kill "$CURL" 2>/dev/null; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
command -v curl    >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }
[ -d /proc ] || { echo "  (atlandi: /proc gerekli — Linux)"; exit 0; }
fail=0

cat > "$TMP/app.lk" <<'LKEOF'
route("SSE", "/events", function($sse) {
    sse::send($sse, "hi", "greet")
    timer::after(2000, function() use ($sse) { sse::close($sse) })   # sunucu-tetikli kapatma
})
LKEOF

"$FCGI" --mode http --port "$PORT" "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; [ -d "/proc/$SRV/fd" ] && ss -tlnp 2>/dev/null | grep -q ":$PORT " && break; sleep 0.25; done
sleep 1
base=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# SSE bağlan (Accept başlığı ŞART — yoksa SSE upgrade tetiklenmez, yanlış-yeşil)
curl -sN -H 'Accept: text/event-stream' "http://127.0.0.1:$PORT/events" >"$TMP/c.out" 2>&1 &
CURL=$!
sleep 1
got=$(head -c 40 "$TMP/c.out" 2>/dev/null | tr -d '\r\n')
conn=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# Sanity: bağlantı gerçekten kuruldu mu (aracın koştuğunu doğrula — Kural 3)
if [ "$conn" -ne $((base+1)) ] || ! printf '%s' "$got" | grep -q 'greet'; then
  echo "  FAIL sanity: SSE bağlantısı kurulmadı (fd $base→$conn, curl='$got') — test SSE yolunu egzersiz etmedi"; fail=1
else
  echo "  OK bağlandı: fd $base→$conn, curl 'greet' aldı"
fi

# Sunucu sse::close (2s) sonrası: fd base'e dönmeli + curl EOF almalı
sleep 2
after=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$CURL" 2>/dev/null; then echo "  FAIL: sse::close sonrası curl HÂLÂ bağlı (soket kapanmadı — istemci asılı)"; fail=1
else echo "  OK curl EOF aldı (soket gerçekten kapandı)"; fi
if [ "$after" -eq "$base" ]; then echo "  OK fd base'e döndü ($after) — sse::close fd'yi serbest bıraktı (sızıntı YOK)"
else echo "  FAIL: sse::close sonrası fd=$after (base=$base) — SIZINTI (fix öncesi davranış)"; fail=1; fi

# Regresyon: saf client-disconnect (server-close yok) hâlâ temizlemeli, çift-close/crash yok
kill "$CURL" 2>/dev/null; CURL=""
for i in 1 2 3; do curl -sN -H 'Accept: text/event-stream' --max-time 1 "http://127.0.0.1:$PORT/events" >/dev/null 2>&1; done
sleep 1
recyc=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$SRV" 2>/dev/null && [ "$recyc" -le $((base+1)) ]; then echo "  OK client-disconnect 3-döngü: fd=$recyc, server ayakta (çift-close/crash yok)"
else echo "  FAIL: client-disconnect regresyonu (fd=$recyc, server $(kill -0 $SRV 2>/dev/null && echo ayakta || echo ÇÖKTÜ))"; fail=1; fi

[ $fail = 0 ] && echo "PASS: SSE sse::close fd-sizintisi" || echo "FAIL: SSE sse::close fd-sizintisi"
exit $fail
