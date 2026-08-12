#!/usr/bin/env bash
# cookie:: güvenli-varsayılan flag guard'ı — lk-fcgi --mode http'ye karşı Set-Cookie başlığını
# doğrular. GÜVENLİ-VARSAYILAN flip (2026-08): HttpOnly + SameSite=Lax varsayılan; Secure değil;
# opt-out options assoc'u ile. POZİTİF-KONTROLLÜ: varsayılan flag'ler kalkarsa /default RED.
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7793}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 0.3; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }

"$FCGI" --mode http --port "$PORT" "$HERE/cookie_flags_app.lk" >/dev/null 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/default" 2>/dev/null && break; sleep 0.2; done

fail=0
# $1=label $2=path $3=grep-pattern-VAR-olmalı  ($4 opsiyonel: OLMAMALI-pattern)
sc() { curl -s -D - -o /dev/null "http://127.0.0.1:$PORT/$1" 2>/dev/null | grep -i '^set-cookie:'; }
chk() {
  local label="$1" got="$2" must="$3" mustnot="${4:-}"
  if ! echo "$got" | grep -qi "$must"; then echo "  FAIL $label: '$must' YOK -> [$got]"; fail=1; return; fi
  if [ -n "$mustnot" ] && echo "$got" | grep -qi "$mustnot"; then echo "  FAIL $label: '$mustnot' OLMAMALI -> [$got]"; fail=1; return; fi
  echo "  OK   $label"
}

D=$(sc default)
chk "varsayilan HttpOnly"      "$D" "HttpOnly"
chk "varsayilan SameSite=Lax"  "$D" "SameSite=Lax"
chk "varsayilan Secure YOK"    "$D" "Path=/" "Secure"           # Secure varsayilan degil

O=$(sc optout)
chk "opt-out HttpOnly YOK"     "$O" "SameSite=Lax" "HttpOnly"   # httponly=false → HttpOnly yok, SameSite kalir

S=$(sc secure)
chk "secure opt-in Secure"     "$S" "Secure"
chk "secure SameSite=Strict"   "$S" "SameSite=Strict"

E=$(sc expires)
chk "pozisyonel Path=/app"     "$E" "Path=/app"
chk "pozisyonel Expires"       "$E" "Expires="
chk "pozisyonel HttpOnly hala" "$E" "HttpOnly"                  # varsayilan flag pozisyonelle de gelir

N=$(sc nosamesite)
chk "samesite opt-out"         "$N" "HttpOnly" "SameSite"       # samesite=false → SameSite yok, HttpOnly kalir

[ $fail = 0 ] && echo "PASS: cookie:: guvenli-varsayilan (HttpOnly+SameSite=Lax) + opt-out" || echo "FAIL: cookie:: flag guard"
exit $fail
