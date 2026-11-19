#!/bin/bash
# jobs:: guard — queue persistence and the MULTI-PROCESS claim race.
#
# WHY: jobs_stdlib.cpp (457 lines) was an unguarded surface and the claim path was written
# as SELECT+UPDATE, with NO transaction/state check BETWEEN them.
# The std::lock_guard in between is only an IN-PROCESS mutex — in FastCGI multi-worker
# each worker is a SEPARATE PROCESS so it gave no protection at all.
# MEASURED (before the fix): 4 processes / 40 jobs -> 34 claims, 29 unique
#   => 5 jobs were given to MORE THAN ONE process (the same e-mail twice, the same payment twice).
# Fix: a single-statement atomic claim (UPDATE ... WHERE id=(SELECT ...) AND
# status='pending' RETURNING ...) + WAL + busy_timeout.
#
# a retry WAITS (c4d1471; also when jobs::fail is called by hand). The flow test sets the base to 1 s:
# a failed job must not be taken again immediately, must be taken once the wait has passed, and be failed on the 3rd attempt.
# (This test was not run in CI; it had stayed red with the old "retake immediately" expectation.)
# Usage: bash jobs_test.sh <lk_path>
set -u
LK="${1:-./build/lk}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"   # cd is done below: absolute path
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
cd "$TMP" || exit 1

cat > push.lk <<'LK'
use jobs
$i = 0
while ($i < 40) { jobs::push("q", "is-" . $i); $i = $i + 1 }
print("ok")
LK
cat > claim.lk <<'LK'
use jobs
$s = ""
$i = 0
while ($i < 30) {
  $j = jobs::next("q")
  if ($j == null) { break }
  $s = $s . $j.id . " "
  $i = $i + 1
}
print($s)
LK
cat > akis.lk <<'LK'
use jobs
# retry: in LOOK a retry WAITS (base LOOK_JOBS_RETRY_SECONDS; here 1 s → 1, 2 s);
# the job must not be taken before the wait, then it must be; once max_retries (3) is used up it is failed (must not retry forever)
jobs::push("r", "x")
$j = jobs::next("r"); jobs::fail($j.id)
$hemen = jobs::next("r")
timer::after(1300, fn() => null); timer::wait(5000)
$j = jobs::next("r"); jobs::fail($j.id)
timer::after(2300, fn() => null); timer::wait(5000)
$j = jobs::next("r"); jobs::fail($j.id)
$son = jobs::next("r")
$rs = jobs::stats("r")
# run_after: the 4th parameter is the DELAY (the 3rd parameter is max_retries — easy to confuse)
jobs::push("d", "gec", 3, 3600)
$erken = jobs::next("d")
# a stuck job: if it is claimed and neither done nor fail is called, recover must take it back
jobs::push("s", "asili")
jobs::next("s")
$kurt = jobs::recover("s", 0)
$tekrar = jobs::next("s")
# a job that was done must not be taken again
jobs::push("o", "bir")
$o = jobs::next("o")
jobs::done($o.id)
$o2 = jobs::next("o")
print(($hemen == null ? "bekledi" : "HEMEN") . "|" . $rs.failed . "|" . ($son == null ? "bitti" : "TEKRAR") . "|" .
      ($erken == null ? "gec" : "ERKEN") . "|" . $kurt . "|" .
      ($tekrar != null ? "kurtarildi" : "YOK") . "|" . ($o2 == null ? "bitti" : "TEKRAR"))
LK

export JOBS_DB="$TMP/jobs.db"
"$LK" push.lk >/dev/null 2>&1
for i in 1 2 3 4; do "$LK" claim.lk > "p$i.txt" 2>/dev/null & done
wait
cat p*.txt | grep -v '^\[' | tr -s ' ' '\n' | grep '[0-9]' | sort -n > all.txt
toplam=$(wc -l < all.txt); tekil=$(sort -u all.txt | wc -l)
if [ "$toplam" = "$tekil" ] && [ "$tekil" -gt 0 ]; then
  echo "  PASS multi-process claim: $toplam claims / $tekil unique (no double processing)"
else
  echo "  FAIL DOUBLE PROCESSING: $toplam claims but $tekil unique jobs"
  echo "       -> claim is not atomic (race between SELECT and UPDATE; an in-process mutex is not enough)"
  fail=1
fi
if [ "$tekil" -ne 40 ]; then
  echo "  FAIL JOB LOSS: only $tekil of 40 jobs could be taken"; fail=1
fi

export JOBS_DB="$TMP/jobs2.db"
out=$(LOOK_JOBS_RETRY_SECONDS=1 "$LK" akis.lk 2>&1 | grep -v '^\[' | tail -1)
bek="bekledi|1|bitti|gec|1|kurtarildi|bitti"
if [ "$out" = "$bek" ]; then
  echo "  PASS flow: a retry waits, retries end in failed, run_after, recover, done"
else
  echo "  FAIL jobs flow: [$out] (expected $bek)"
  echo "       order: waited|failed-count|failed-not-retaken|delayed-job|recover-count|recovered-taken|done-not-retaken"
  fail=1
fi

# COLD START: with NO DB at all, N processes create the schema at the same time.
# If busy_timeout is not set RIGHT AFTER sqlite3_open, the WAL switch and
# create_schema() immediately return SQLITE_BUSY while asking for an exclusive lock and the processes
# CRASH with 'schema hatasi: database is locked'. Worse: dying processes
# MASK the claim race (the race is not visible when few processes remain).
rm -f "$TMP/cold.db"*
export JOBS_DB="$TMP/cold.db"
cat > cold.lk <<'LK'
use jobs
$j = jobs::next("q")
print("ok")
LK
for i in 1 2 3 4 5 6; do "$LK" cold.lk > "c$i.out" 2> "c$i.err" & done
wait
kilit=0
for i in 1 2 3 4 5 6; do grep -qi "locked" "c$i.err" 2>/dev/null && kilit=$((kilit+1)); done
if [ "$kilit" = "0" ]; then
  echo "  PASS cold start: 6 processes created the schema concurrently, no lock error"
else
  echo "  FAIL cold start: $kilit of 6 processes crashed with 'database is locked'"
  echo "       -> sqlite3_busy_timeout() must be called RIGHT AFTER sqlite3_open (before WAL/schema)"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: jobs::" || echo "FAIL: jobs::"
exit $fail
