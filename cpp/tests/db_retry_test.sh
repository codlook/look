#!/usr/bin/env bash
# db_retry_test.sh — PG kör-retry SESSİZ ÇİFT-YAZMA guard'ı (53. bug).
#
# PostgresClient::query()/execute() her std::runtime_error'da bağlantıyı yeniden
# kurup sorguyu KOŞULSUZ yeniden çalıştırıyordu. Bağlantı isteği gönderdikten
# SONRA ama yanıt okunmadan koparsa (proxy/failover/blip), sunucu INSERT'i
# işlemiş olabilir; retry onu İKİNCİ kez çalıştırır → sessiz çift-yazma
# (çift ödeme/çift sipariş) — bu oturumun ekseni.
#
# Fix: retry YALNIZ gönderim başarısızsa (PgSendError = istek sunucuya ulaşmadı,
# güvenli). Yanıt-okuma kopması retry EDİLMEZ → app fail-loud hata alır.
#
# Kanıt: sahte-PG "commit_kopar" — INSERT'i sayar, ilk seferinde yanıt vermeden
# soketi kapatır. İstemci retry ederse sayaç 2 (çift-yazma); fix'te 1 + app hata.
set -u
LK="${1:-./build/lk}"
PY="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${2:-55443}"
CF="$(mktemp)"; trap 'rm -f "$CF" "$CF".lk; [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null' EXIT

PG_COUNT_FILE="$CF" "$PY" "$HERE/fake_pg_server.py" commit_kopar "$PORT" >/dev/null 2>&1 &
SRV=$!
sleep 1
cat > "$CF".lk <<LK
use db
\$c = db::connect("postgres://u:p@127.0.0.1:$PORT/d")
db::query(\$c, "INSERT INTO t(x) VALUES(1)")
print("done")
LK
out=$(PG_COUNT_FILE="$CF" timeout 20 "$LK" "$CF".lk 2>&1)
kill "$SRV" 2>/dev/null; SRV=""
n=$(cat "$CF" 2>/dev/null || echo "?")

fail=0
if [ "$n" != "1" ]; then
  echo "  FAIL cift-yazma: INSERT sunucuya $n kez ulasti (1 bekleniyordu) — kor retry statement'i tekrar isliyor"
  fail=1
else
  echo "  PASS: post-send kopmada INSERT tek kez ulasti (kor retry yok)"
fi
# App fail-loud olmali (sessiz 'done' DEGIL)
if echo "$out" | grep -q "done" && ! echo "$out" | grep -qi "connection lost\|error"; then
  echo "  FAIL fail-loud: app hata gormeden 'done' dedi (sessiz basari — kopma gizlendi)"
  fail=1
else
  echo "  PASS: app kopmayi hata olarak gordu (fail-loud)"
fi

[ $fail = 0 ] && echo "PASS: db retry (53 — PG sessiz cift-yazma)" || echo "FAIL: db retry"
exit $fail
