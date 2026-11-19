#!/usr/bin/env bash
# db_retry_test.sh — PG blind-retry SILENT DOUBLE-WRITE guard (bug 53).
#
# PostgresClient::query()/execute() reconnected on every std::runtime_error
# and re-ran the query UNCONDITIONALLY. If the connection drops after the request is sent
# but BEFORE the reply is read (proxy/failover/blip), the server may have
# processed the INSERT; the retry runs it a SECOND time → a silent double-write
# (double payment/double order) — the axis of this session.
#
# Fix: retry ONLY if the send failed (PgSendError = the request did not reach the server,
# safe). A drop while reading the reply is NOT retried → the app gets a fail-loud error.
#
# Proof: a fake PG "commit_kopar" — counts the INSERT, the first time closes
# the socket without replying. If the client retries the counter is 2 (double-write); with the fix 1 + an app error.
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
# The app must be fail-loud (NOT a silent 'done')
if echo "$out" | grep -q "done" && ! echo "$out" | grep -qi "connection lost\|error"; then
  echo "  FAIL fail-loud: app hata gormeden 'done' dedi (sessiz basari — kopma gizlendi)"
  fail=1
else
  echo "  PASS: app kopmayi hata olarak gordu (fail-loud)"
fi

[ $fail = 0 ] && echo "PASS: db retry (53 — PG sessiz cift-yazma)" || echo "FAIL: db retry"
exit $fail
