#!/usr/bin/env bash
# When a command-line script ends, a parallel() task is named in the "still running" warning only
# if it still had work that leaves a trace (output, channel, file, network, database) — not when
# all it had left was to return after its last send(). That false warning came on every run of
# the common patterns (one send, sends in a loop, a send in an if branch, a while over count()).
# The script never waits for a task's work: a task blocked in receive(), an endless loop and an
# send() into a full channel nobody reads are named, and the script still ends at once.
# Usage: parallel_exit_warning_test.sh <lk> [runs]
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk> [runs]}")" && pwd)/$(basename "$1")"
RUNS="${2:-20}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0

cat > "$T/single.lk" <<'EOF'
$ch = channel(1)
parallel(function() { send($ch, 41 + 1) })
print(receive($ch))
EOF
cat > "$T/loop.lk" <<'EOF'
function work($x) { return $x * 2 }
$ch = channel(8)
parallel(function() {
  foreach ([1, 2, 3] as $x) { send($ch, work($x)) }
})
$s = 0
for ($i = 0; $i < 3; $i++) { $s = $s + receive($ch) }
print($s)
EOF
cat > "$T/ifb.lk" <<'EOF'
$ch = channel(1)
$n = 5
parallel(function() {
  if ($n > 3) { send($ch, "big") } else { send($ch, "small") }
})
print(receive($ch))
EOF
cat > "$T/whilecount.lk" <<'EOF'
$items = [1, 2, 3, 4]
$ch = channel(8)
parallel(function() {
  $i = 0
  while ($i < count($items)) { send($ch, $items[$i] * 10); $i = $i + 1 }
})
$s = 0
for ($k = 0; $k < 4; $k++) { $s = $s + receive($ch) }
print($s)
EOF
cat > "$T/reads_after.lk" <<'EOF'
use cache
$ch = channel(1)
parallel(function() { send($ch, 1); $v = cache::get("k") })
print(receive($ch))
EOF
# Real work left: named every time.
cat > "$T/real.lk" <<'EOF'
$ch = channel(1)
parallel(function() { send($ch, 1); $x = 0; for ($i = 0; $i < 30000000; $i++) { $x = $x + 1 }; print("late") })
print(receive($ch))
EOF
cat > "$T/blocked.lk" <<'EOF'
$ch = channel(1)
parallel(function() { $v = receive($ch) })
print("end")
EOF
cat > "$T/endless.lk" <<'EOF'
parallel(function() { $x = 0; while (true) { $x = $x + 1 } })
print("end")
EOF
cat > "$T/send_blocked.lk" <<'EOF'
$ch = channel(1)
parallel(function() { send($ch, 1); send($ch, 2) })
print("end")
EOF

# $1 file, $2 expected stdout, $3 want warnings out of RUNS (0 or all)
check() {
  local w=0 bad=0 to=0 out rc
  for i in $(seq 1 "$RUNS"); do
    out=$(cd "$T" && timeout 20 "$LK" "$1" 2>"$T/err"); rc=$?
    [ $rc = 124 ] && to=$((to+1))
    grep -q 'still running' "$T/err" && w=$((w+1))
    [ "$out" = "$2" ] || bad=$((bad+1))
  done
  if [ $to = 0 ] && [ $bad = 0 ] && [ $w = "$3" ]; then echo "  OK   $1: warned $w/$RUNS"
  else echo "  FAIL $1: warned $w/$RUNS (want $3), wrong output $bad, hung $to"; fail=1; fi
}
check single.lk 42 0
check loop.lk 12 0
check ifb.lk big 0
check whilecount.lk 100 0
check reads_after.lk 1 0
check real.lk 1 "$RUNS"
check blocked.lk end "$RUNS"
check endless.lk end "$RUNS"
check send_blocked.lk end "$RUNS"

[ $fail = 0 ] && echo "PASS: only tasks with work left are named when a script ends" || { echo "FAIL: parallel exit warning"; exit 1; }
