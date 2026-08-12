#!/usr/bin/env bash
# Bounded-concurrency HANG regresyon smoke-test'i (worker-pool + fiber dispatch).
#
# ── Neyi koruyor ──────────────────────────────────────────────────────────────
# lk-fcgi --mode http iki dispatch modeliyle gelir:
#   • worker-pool (DEFAULT)          — N worker thread, her biri accept+handle bloklar.
#   • fiber burst (LOOK_FIBER_DISPATCH=1, Linux) — SO_REUSEPORT + fiber acceptor.
# Geçmişte fiber modu c=100'de HANG ediyordu: acceptor keep-alive fiber'larını
# beklerken yeni bağlantıları accept etmiyordu. D Fix #2 (acceptor artık bir fiber +
# 15s idle-timeout) düzeltti — ama YÜK-REGRESYON guard'ı yoktu. Bu test o guard.
#
# ── POZİTİF-KONTROL ───────────────────────────────────────────────────────────
# Test GERÇEKTEN eşzamanlılığı egzersiz eder: CONC bağlantı AYNI ANDA açılır, her
# biri keep-alive üzerinde PERCON istek yollar (accept + keep-alive-recv ekseni —
# tam da hang'in kök nedeni). Bir hang OLSAYDI: aç kalan bağlantılar CONN_TIMEOUT
# içinde işlerini bitiremez → eksik 200 sayısı VE/VEYA yükten sonra probe_alive()
# false → FAIL. Tek-istek smoke'un yakalayamayacağı sınıfı yakalar.
# (Bunu kanıtlamak için: workers=1 + düşük timeout ile pool'u aç bırakıp FAIL
#  gördüğünü doğrula — README'de değil, elle pozitif-kontrol adımı.)
#
# ── FLAKY UYARISI ─────────────────────────────────────────────────────────────
# Yük-testi doğası gereği makine-yüküne duyarlı. Bu harness DÜŞÜK concurrency
# (default c=50) + CÖMERT timeout (8s/conn) ile flaky payını düşürür ama bir hang
# regresyonunu hâlâ yakalar (hang saniyeler değil SÜRESİZ askıdır). Yine de
# 2-core paylaşımlı CI runner'da c=100 gürültü verebilir; oraya c=50 önerilir.
#
# Kullanım: bash concurrency_smoke.sh [lk-fcgi_yolu] [concurrency] [reqs_per_conn]
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
FCGI="${1:-$DIR/../build/lk-fcgi}"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"
CONC="${2:-50}"
PERCON="${3:-20}"
PORT=7402
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Minik, CPU-hafif app: /health hızlı 200 döner (hang'i izole eder — app yavaşlığı
# değil dispatch'i ölçeriz).
cat > "$TMP/app.lk" <<'LK'
route("GET", "/health", function() {
  return response::text("ok")
})
LK

cd "$TMP"

run_mode() {
  local label="$1"; shift
  local env_pfx="$1"; shift
  # env_pfx: "" (pool) veya "LOOK_FIBER_DISPATCH=1"
  echo "[$label] sunucu başlatılıyor..."
  if [ -n "$env_pfx" ]; then export $env_pfx; fi
  "$FCGI" --mode http --port $PORT app.lk >"$TMP/srv.log" 2>&1 &
  local srv=$!
  # Hazır olana dek bekle (curl ile /health 200)
  local up=0
  for i in $(seq 1 40); do
    if curl -s --max-time 1 "http://127.0.0.1:$PORT/health" 2>/dev/null | grep -q ok; then up=1; break; fi
    sleep 0.25
  done
  if [ "$up" != "1" ]; then
    echo "  FAIL [$label]: sunucu ayağa kalkmadı"; sed 's/^/    srv: /' "$TMP/srv.log"
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    if [ -n "$env_pfx" ]; then unset ${env_pfx%%=*}; fi
    return 1
  fi
  python3 "$DIR/concurrency_smoke.py" $PORT $CONC $PERCON
  local rc=$?
  kill $srv 2>/dev/null; wait $srv 2>/dev/null; sleep 0.5
  if [ -n "$env_pfx" ]; then unset ${env_pfx%%=*}; fi
  return $rc
}

fail=0
run_mode "worker-pool" ""                       || fail=1
run_mode "fiber"       "LOOK_FIBER_DISPATCH=1"  || fail=1

if [ "$fail" = "0" ]; then
  echo "TÜMÜ PASS: her iki dispatch modu c=$CONC'de hang etmedi."
else
  echo "BAŞARISIZ: en az bir mod hang/eksik-yanıt gösterdi."
fi
exit $fail
