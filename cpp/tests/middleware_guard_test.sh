#!/usr/bin/env bash
# before_route prefix-guard guard — against lk-fcgi --mode http. The prefix guard is now dogfood's
# authentication mechanism (9→1 refactor); if it breaks, /tasks (here /admin) opens without auth =
# SILENT AUTHORIZATION BYPASS. WITH A POSITIVE CONTROL: the variant without the guard returns 200 for /admin → RED discrimination.
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7795}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRV=""; SRV2=""
cleanup(){ for p in "$SRV" "$SRV2"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; sleep 0.3;
           for p in "$SRV" "$SRV2"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (skipped: curl required)"; exit 0; }

"$FCGI" --mode http --port "$PORT" "$HERE/middleware_guard_app.lk" >/dev/null 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/public/hello" 2>/dev/null && break; sleep 0.2; done

fail=0
code(){ curl -s -o /dev/null -w "%{http_code}" ${3:-} "http://127.0.0.1:$PORT/$1" 2>/dev/null; }
chk(){ local label="$1" got="$2" want="$3"; if [ "$got" = "$want" ]; then echo "  OK   $label ($got)";
       else echo "  FAIL $label: $got expected $want"; fail=1; fi; }

chk "public is open"          "$(code public/hello)"                     "200"
chk "admin without header -> 403"  "$(code admin/secret '' '')"              "403"
chk "admin correct token -> 200" "$(code admin/secret '' '-H X-Token:sekret')" "200"
chk "admin wrong token -> 403" "$(code admin/secret '' '-H X-Token:yanlis')" "403"
chk "admin NESTED without header -> 403" "$(code admin/deep/x)"             "403"   # the prefix closes nested paths too

# ── POSITIVE CONTROL: before_route SABOTAGED (no-op) → /admin OPENS without auth (200) ──
# This proves the test really goes RED if the guard breaks (silent-bypass detection).
SAB="$(mktemp --suffix=.lk)"
cat > "$SAB" <<'LK'
use string
before_route(function() { })   # SABOTAGE: no guard
route("GET", "/admin/secret", function() { response::json(["zone" => "admin"]) })
LK
"$FCGI" --mode http --port "$((PORT+1))" "$SAB" >/dev/null 2>&1 &
SRV2=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$((PORT+1))/admin/secret" 2>/dev/null && break; sleep 0.2; done
sab=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$((PORT+1))/admin/secret" 2>/dev/null)
rm -f "$SAB"
if [ "$sab" = "200" ]; then echo "  OK   positive control: /admin without the guard = 200 (the test discriminates)";
else echo "  FAIL positive control: /admin without the guard = $sab (should be 200 — the test may be vacuous)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: before_route prefix guard (403/200 + nested + positive control)" || echo "FAIL: middleware guard"
exit $fail
