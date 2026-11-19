#!/bin/bash
# SSE sse::close() fd-leak regression guard (the persisted form of the 957d11e fix).
#
# BUG (measured, adversarial bug hunt 2026-08-11): when the server called sse::close() the fd/sse_clients/
# epoll registration LEAKED until the client's TCP dropped (close_conn only marked it closed, the real
# cleanup was deferred to the event loop's next READ event). Also, since the socket was not closed, the
# client did not know the stream had ended (it hung). Fix: close_conn → loop->post(close_sse)
# (loop-thread cleanup) + an own-once guard (no double close).
#
# POSITIVE CONTROL (Rule 1): BEFORE the fix this test was RED — after sse::close fd = base+1
# (leak). AFTER the fix fd = base (freed) + curl gets EOF (the socket really closes).
# Detector = the /proc/PID/fd counter (the fd lifetime race is TSan-blind, so this is the right tool).
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
    timer::after(2000, function() use ($sse) { sse::close($sse) })   # server-triggered close
})
LKEOF

"$FCGI" --mode http --port "$PORT" "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; [ -d "/proc/$SRV/fd" ] && ss -tlnp 2>/dev/null | grep -q ":$PORT " && break; sleep 0.25; done
sleep 1
base=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# SSE connect (the Accept header is REQUIRED — without it the SSE upgrade is not triggered, false green)
curl -sN -H 'Accept: text/event-stream' "http://127.0.0.1:$PORT/events" >"$TMP/c.out" 2>&1 &
CURL=$!
sleep 1
got=$(head -c 40 "$TMP/c.out" 2>/dev/null | tr -d '\r\n')
conn=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# Sanity: was the connection really established (verify the tool ran — Rule 3)
if [ "$conn" -ne $((base+1)) ] || ! printf '%s' "$got" | grep -q 'greet'; then
  echo "  FAIL sanity: SSE bağlantısı kurulmadı (fd $base→$conn, curl='$got') — test SSE yolunu egzersiz etmedi"; fail=1
else
  echo "  OK bağlandı: fd $base→$conn, curl 'greet' aldı"
fi

# After the server's sse::close (2s): fd must return to base + curl must get EOF
sleep 2
after=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$CURL" 2>/dev/null; then echo "  FAIL: sse::close sonrası curl HÂLÂ bağlı (soket kapanmadı — istemci asılı)"; fail=1
else echo "  OK curl EOF aldı (soket gerçekten kapandı)"; fi
if [ "$after" -eq "$base" ]; then echo "  OK fd base'e döndü ($after) — sse::close fd'yi serbest bıraktı (sızıntı YOK)"
else echo "  FAIL: sse::close sonrası fd=$after (base=$base) — SIZINTI (fix öncesi davranış)"; fail=1; fi

# Regression: a pure client-disconnect (no server-close) must still clean up, no double-close/crash
kill "$CURL" 2>/dev/null; CURL=""
for i in 1 2 3; do curl -sN -H 'Accept: text/event-stream' --max-time 1 "http://127.0.0.1:$PORT/events" >/dev/null 2>&1; done
sleep 1
recyc=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$SRV" 2>/dev/null && [ "$recyc" -le $((base+1)) ]; then echo "  OK client-disconnect 3-döngü: fd=$recyc, server ayakta (çift-close/crash yok)"
else echo "  FAIL: client-disconnect regresyonu (fd=$recyc, server $(kill -0 $SRV 2>/dev/null && echo ayakta || echo ÇÖKTÜ))"; fail=1; fi

[ $fail = 0 ] && echo "PASS: SSE sse::close fd-sizintisi" || echo "FAIL: SSE sse::close fd-sizintisi"
exit $fail
