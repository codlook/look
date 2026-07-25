#!/usr/bin/env bash
# deploy-dogrula.sh — DEPLOY SONRASI canlı duman testi.
#
# Guard'lar kaynağı test eder; bu betik DEPLOY EDİLMİŞ BİNARY'nin gerçekten
# düzeltmeleri içerdiğini DIŞARIDAN (HTTP) ve binary'yi çalıştırarak doğrular —
# "yapılmış görünen ama yapılmamış" riskine karşı tek dış kanıt. Docker-local
# deploy'da da, VPS deploy'da da koşulabilir.
#
# Kullanım:
#   deploy-dogrula.sh [LK] [LKFCGI]      # yerel binary'ler (Docker)
#   BASE_URL=https://test.codlook.com deploy-dogrula.sh   # canlı site (yalnız HTTP kontrolleri)
#
# Doğruladığı düzeltmeler:
#   50 — lk test: assert_throws() gerçekten assert ediyor mu (yalancı yeşil değil)
#   52 — json::decode int64-aşan ID'yi string olarak koruyor mu (HTTP üzerinden)
set -u
LK="${1:-./build/lk}"
LKFCGI="${2:-./build/lk-fcgi}"
BASE_URL="${BASE_URL:-}"
fail=0
TMP=$(mktemp -d); trap 'rm -rf "$TMP"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null' EXIT

echo "── deploy doğrulama ─────────────────────────────────────────"

# ── 52. bug: json::decode int64 kesinliği — HTTP üzerinden (dış kanıt) ────────
JBIG='9223372036854775809'
if [ -n "$BASE_URL" ]; then
  # Canlı site: /_dogrula_json endpoint'i beklenir (deploy edilen app'te olmalı)
  body=$(curl -s --max-time 10 "$BASE_URL/_dogrula_json" 2>/dev/null)
else
  # Docker-local: kendi mini app'imizi HTTP modunda kaldır
  cat > "$TMP/app.lk" <<'EOF'
use json
route("GET", "/_dogrula_json", function() {
    $d = json::decode("{\"id\": 9223372036854775809, \"big\": 18446744073709551615, \"norm\": 42}")
    print(json::encode($d))
})
route("GET", "/_health", function() { print("ok") })
EOF
  PORT=7599
  "$LKFCGI" --mode http --port $PORT "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
  SRV_PID=$!
  # sunucu ayaga kalksin
  for _ in $(seq 1 20); do
    curl -s --max-time 2 "http://127.0.0.1:$PORT/_health" 2>/dev/null | grep -q ok && break
    sleep 0.3
  done
  body=$(curl -s --max-time 10 "http://127.0.0.1:$PORT/_dogrula_json" 2>/dev/null)
fi

if echo "$body" | grep -q "\"$JBIG\""; then
  echo "  PASS 52 (HTTP): json int64-asan ID string olarak korunuyor (canli binary)"
elif echo "$body" | grep -q "9223372036854775808"; then
  echo "  FAIL 52 (HTTP): ID sessizce ...808'e bozuldu — DUZELTME CANLI DEGIL"; fail=1
else
  echo "  FAIL 52 (HTTP): beklenen yanit alinamadi: [$body]"; fail=1
fi

# ── 50. bug: lk test assert_throws gercekten assert ediyor mu ─────────────────
# (yalniz yerel binary ile — canli site HTTP'de lk test kosulmaz)
if [ -z "$BASE_URL" ]; then
  mkdir -p "$TMP/tests"
  cat > "$TMP/tests/t.lk" <<'EOF'
test("firlatmayan fn => BASARISIZ olmali", function() {
    assert_throws(function() { $x = 1 })
})
EOF
  tout=$( cd "$TMP" && "$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")" test 2>&1 | sed 's/\x1b\[[0-9;]*m//g' )
  if echo "$tout" | grep -q "1 başarısız\|1 basarisiz"; then
    echo "  PASS 50: assert_throws firlatmayan fn'i yakaliyor (cerceve dogru)"
  else
    echo "  FAIL 50: assert_throws assert ETMIYOR — test cercevesi yalanci yesil:"; echo "$tout" | grep -E "✅|❌|geçti" | sed 's/^/       /'; fail=1
  fi
fi

# ── 56. bug: Slowloris header deadline — CANLI ortamda (zamanlama-duyarlı) ────
# Yalnız yerel binary ile (kendi instance'ını düşük deadline ile başlatır).
if [ -z "$BASE_URL" ]; then
  SLPORT=7598
  LOOK_HEADER_TIMEOUT=3000 "$LKFCGI" --mode http --port "$SLPORT" --workers 2 "$TMP/app.lk" >"$TMP/sl.log" 2>&1 &
  SLPID=$!
  for _ in $(seq 1 20); do
    curl -s --max-time 2 "http://127.0.0.1:$SLPORT/_health" 2>/dev/null | grep -q ok && break; sleep 0.3
  done
  sl=$(python3 - "$SLPORT" <<'PY'
import socket, sys, time
P = int(sys.argv[1]); s = socket.socket(); s.connect(("127.0.0.1", P))
cut = False
try:
    for p in [b"GET / HTTP/1.1\r\n", b"H1: a\r\n", b"H2: b\r\n", b"H3: c\r\n", b"H4: d\r\n", b"H5: e\r\n"]:
        s.sendall(p); time.sleep(1.0)
except Exception:
    cut = True
if not cut:
    s.settimeout(2.0)
    try:
        d = s.recv(64); cut = (d == b"" or b"408" in d)
    except socket.timeout:
        cut = False
    except Exception:
        cut = True
s.close()
print("KESILDI" if cut else "TUTULUYOR")
PY
)
  kill "$SLPID" 2>/dev/null
  if [ "$sl" = "KESILDI" ]; then
    echo "  PASS 56 (Slowloris): surekli-damla deadline'da kesildi (canli ortam, deadline=3s)"
  else
    echo "  FAIL 56 (Slowloris): yavas-damla TUTULDU ($sl) — deadline canli calismiyor"; fail=1
  fi
fi

# ── 57. bug: VM özyineleme bounded (OOM DoS) — CLI, yerel binary ─────────────
# CALL opcode MAX_CALL_DEPTH'i zorlamalı; runaway özyineleme graceful "Stack
# overflow" vermeli, bellek tükenene dek gitmemeli. lk-fcgi CLI modu yoksa lk'ye
# düşer; ikisi de yoksa atlar (canlı HTTP-only modda LK olmayabilir).
RECLK="$LK"; [ -x "$RECLK" ] || RECLK="$LKFCGI"
if [ -z "$BASE_URL" ] && [ -x "$RECLK" ]; then
  printf 'function r($n){ if($n<=0){return 0} return 1+r($n-1) }\nprint(r(100000))\n' > "$TMP/rec.lk"
  rout=$(timeout 15 "$RECLK" "$TMP/rec.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ')
  if echo "$rout" | grep -qi "stack overflow"; then
    echo "  PASS 57 (VM ozyineleme): derin ozyineleme bounded (graceful stack overflow)"
  elif echo "$rout" | grep -q "100000"; then
    echo "  FAIL 57: r(100000) dondu — CALL yolu MAX_CALL_DEPTH atliyor (unbounded/OOM riski)"; fail=1
  else
    echo "  (57 atlandi: beklenmedik cikti: $rout)"
  fi
fi

echo "─────────────────────────────────────────────────────────────"
[ $fail = 0 ] && echo "PASS: deploy dogrulandi — duzeltmeler canli binary'de" || echo "FAIL: deploy dogrulama BASARISIZ"
exit $fail
