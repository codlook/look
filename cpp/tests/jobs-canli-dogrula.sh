#!/usr/bin/env bash
# jobs-canli-dogrula.sh — jobs claim race + cold start, in a REAL environment.
#
# 46 (atomic claim) and 47 (busy_timeout cold start) were environment-sensitive findings;
# the race window depends on file system/kernel speed. This script is run in the deployed
# environment (e.g. test.codlook.com AlmaLinux 8) and measures double processing and lock
# errors. Default parameters: 6 processes × 8 rounds = 48 claims.
#
# Usage: jobs-canli-dogrula.sh <lk> [nproc=6] [rounds=8]
set -u
LK="${1:-./build/lk}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"   # cd is done below: absolute path
NPROC="${2:-6}"; ROUNDS="${3:-8}"; TOTAL=$((NPROC*ROUNDS))
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1
fail=0

cat > push.lk <<LK
use jobs
\$i = 0
while (\$i < $TOTAL) { jobs::push("q", "is-" . \$i); \$i = \$i + 1 }
print("pushed")
LK
cat > claim.lk <<LK
use jobs
\$i = 0
while (\$i < $ROUNDS) {
  \$j = jobs::next("q")
  if (\$j == null) { break }
  print(\$j.id)
  \$i = \$i + 1
}
LK

export JOBS_DB="$TMP/jobs.db"
"$LK" push.lk >/dev/null 2>&1
for i in $(seq 1 "$NPROC"); do "$LK" claim.lk >"p$i.out" 2>"p$i.err" & done
wait
cat p*.out 2>/dev/null | grep -E '^[0-9]+$' | sort -n > claims.txt
locks=$(cat p*.err 2>/dev/null | grep -ci "locked")
total=$(wc -l < claims.txt); uniq=$(sort -u claims.txt | wc -l); dupe=$((total - uniq))
echo "  claim yarisi: $total claim / $uniq tekil / $dupe cift / $locks kilit-hatasi  (hedef $TOTAL/$TOTAL/0/0)"
[ "$total" = "$TOTAL" ] && [ "$dupe" = 0 ] && [ "$locks" = 0 ] || { echo "  FAIL: claim yarisi hedefi tutmadi"; fail=1; }

# Cold start: with no DB at all, NPROC processes create the schema at the same time.
rm -f "$TMP/cold.db"*
export JOBS_DB="$TMP/cold.db"
cat > cold.lk <<'LK'
use jobs
$j = jobs::next("q")
print("ok")
LK
for i in $(seq 1 "$NPROC"); do "$LK" cold.lk >"c$i.out" 2>"c$i.err" & done
wait
ckilit=0; cok=0
for i in $(seq 1 "$NPROC"); do
  grep -qi "locked" "c$i.err" 2>/dev/null && ckilit=$((ckilit+1))
  grep -q "ok" "c$i.out" 2>/dev/null && cok=$((cok+1))
done
echo "  soguk acilis: $NPROC surec / $cok basarili / $ckilit kilit-hatasi  (hedef $NPROC/$NPROC/0)"
[ "$ckilit" = 0 ] && [ "$cok" = "$NPROC" ] || { echo "  FAIL: soguk acilis kilit hatasi"; fail=1; }

[ $fail = 0 ] && echo "PASS: jobs canli (46 atomik claim + 47 soguk acilis)" || echo "FAIL: jobs canli"
exit $fail
