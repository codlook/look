#!/usr/bin/env bash
# before_route prefix-guard guard'ı — lk-fcgi --mode http'ye karşı. Prefix guard artık dogfood'un
# kimlik doğrulama mekanizması (9→1 refactor); kırılırsa /tasks (burada /admin) auth'suz açılır =
# SESSİZ YETKİ ATLATMASI. POZİTİF-KONTROLLÜ: guard'sız varyant /admin 200 döndürür → RED discriminasyonu.
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7795}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRV=""; SRV2=""
cleanup(){ for p in "$SRV" "$SRV2"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; sleep 0.3;
           for p in "$SRV" "$SRV2"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }

"$FCGI" --mode http --port "$PORT" "$HERE/middleware_guard_app.lk" >/dev/null 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/public/hello" 2>/dev/null && break; sleep 0.2; done

fail=0
code(){ curl -s -o /dev/null -w "%{http_code}" ${3:-} "http://127.0.0.1:$PORT/$1" 2>/dev/null; }
chk(){ local label="$1" got="$2" want="$3"; if [ "$got" = "$want" ]; then echo "  OK   $label ($got)";
       else echo "  FAIL $label: $got beklenen $want"; fail=1; fi; }

chk "public serbest"          "$(code public/hello)"                     "200"
chk "admin header YOK -> 403"  "$(code admin/secret '' '')"              "403"
chk "admin dogru token -> 200" "$(code admin/secret '' '-H X-Token:sekret')" "200"
chk "admin yanlis token -> 403" "$(code admin/secret '' '-H X-Token:yanlis')" "403"
chk "admin NESTED header YOK -> 403" "$(code admin/deep/x)"             "403"   # prefix nested'i de kapatir

# ── POZİTİF KONTROL: before_route SABOTE (no-op) → /admin auth'suz AÇILIR (200) ──
# Bu, guard kırılırsa testin gerçekten RED vereceğini kanıtlar (sessiz-atlatma tespiti).
SAB="$(mktemp --suffix=.lk)"
cat > "$SAB" <<'LK'
use string
before_route(function() { })   # SABOTE: guard yok
route("GET", "/admin/secret", function() { response::json(["zone" => "admin"]) })
LK
"$FCGI" --mode http --port "$((PORT+1))" "$SAB" >/dev/null 2>&1 &
SRV2=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$((PORT+1))/admin/secret" 2>/dev/null && break; sleep 0.2; done
sab=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$((PORT+1))/admin/secret" 2>/dev/null)
rm -f "$SAB"
if [ "$sab" = "200" ]; then echo "  OK   pozitif-kontrol: guard'siz /admin=200 (test discriminador)";
else echo "  FAIL pozitif-kontrol: guard'siz /admin=$sab (200 olmali — test vacuous olabilir)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: before_route prefix guard (403/200 + nested + pozitif-kontrol)" || echo "FAIL: middleware guard"
exit $fail
