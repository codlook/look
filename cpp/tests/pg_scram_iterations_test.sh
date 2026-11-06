#!/usr/bin/env bash
# PostgreSQL SCRAM: a server that asks for an absurd iteration count is refused at once.
#
# The iteration count comes from the server. A malicious server (or someone in between, without
# TLS) sending i=2147483647 kept the connecting thread in PBKDF2 for a very long time — every
# request that opened a connection stuck. Over 1,000,000 iterations (PostgreSQL's default is
# 4096, OWASP recommends 600,000) the connection is refused with the reason.
# Usage: pg_scram_iterations_test.sh <lk>   (needs python3)
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-55441}"
T="$(mktemp -d)"; s=""; trap '[ -n "$s" ] && kill $s 2>/dev/null; rm -rf "$T"' EXIT
python3 "$HERE/fake_pg_server.py" scram_iter "$PORT" > "$T/pg.log" 2>&1 & s=$!
for i in $(seq 1 50); do (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break; sleep 0.1; done
cat > "$T/c.lk" <<LK
use db
try { \$c = db::connect("postgres://u:secret@127.0.0.1:$PORT/d"); db::query(\$c, "SELECT 1", []); print("connected") }
catch (\$e) { print("refused: " . \$e) }
LK
st=$(date +%s); out="$(cd "$T" && timeout 30 "$LK" c.lk 2>&1 | grep -v '^\[20' | tail -1)"; rc=$?; dt=$(( $(date +%s) - st ))
echo "  ..   $out (${dt} s)"
case "$out" in
  *"SCRAM iterations (limit 1000000)"*) [ $dt -lt 5 ] && { echo "PASS: an absurd SCRAM iteration count is refused at once"; exit 0; } ;;
esac
echo "FAIL: SCRAM iteration count from the server is not limited (${dt} s: $out)"; exit 1
