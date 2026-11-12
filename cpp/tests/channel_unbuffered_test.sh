#!/usr/bin/env bash
# channel(0) is unbuffered, as the docs say and as in Go: send() waits until a receive() takes the
# value, so a producer never runs ahead of its consumer (it used to be an unlimited buffer on the
# command line and 1,048,576 places on the web). channel() keeps its 128 places; a negative size is
# an error everywhere.
# When the script and every parallel() task wait on channels and nothing else (no timer) could
# send or receive, the script stops with a "deadlock" error instead of hanging for ever — as Go
# does ("all goroutines are asleep - deadlock!").
# Usage: channel_unbuffered_test.sh <lk> <lk-fcgi>
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
T=$(mktemp -d); pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
fail=0
# $1 label, $2 file, $3 want rc, $4 grep pattern the output must contain
run() {
  local out rc
  out=$(cd "$T" && timeout 20 "$LK" "$2" 2>&1); rc=$?
  if [ "$rc" = "$3" ] && echo "$out" | grep -qE "$4"; then echo "  OK   $1"
  else echo "  FAIL $1: rc=$rc (want $3)"; echo "$out" | head -6 | sed 's/^/       /'; fail=1; fi
}

# A producer on an unbuffered channel never runs ahead: at most one value is ever waiting.
cat > "$T/rendezvous.lk" <<'EOF'
$ch = channel(0)
parallel(function() { for ($i = 0; $i < 20000; $i++) { send($ch, $i) } })
$most = 0
$sum = 0
for ($i = 0; $i < 20000; $i++) {
  $n = chan_size($ch)
  if ($n > $most) { $most = $n }
  $sum = $sum + receive($ch)
}
print("most=" . $most . " sum=" . $sum)
EOF
run "channel(0): the producer waits for the consumer" rendezvous.lk 0 '^most=[01] sum=199990000$'

cat > "$T/default.lk" <<'EOF'
$ch = channel()
for ($i = 0; $i < 128; $i++) { send($ch, $i) }
print("buffered " . chan_size($ch))
EOF
run "channel() keeps 128 places" default.lk 0 '^buffered 128$'

cat > "$T/negative.lk" <<'EOF'
$ch = channel(-1)
EOF
run "a negative size is an error" negative.lk 1 'cannot be negative'

# Deadlocks: reported, never a hang.
cat > "$T/self_send.lk" <<'EOF'
$ch = channel(0)
send($ch, 1)
print(receive($ch))
EOF
run "send on channel(0) with no receiver at all" self_send.lk 1 'deadlock'

cat > "$T/recv_nothing.lk" <<'EOF'
function wait_for($ch) {
  return receive($ch)
}
$ch = channel(1)
print(wait_for($ch))
EOF
run "receive with no sender left (with the call chain)" recv_nothing.lk 1 'deadlock.*|at wait_for \(line 5\)'

cat > "$T/both_wait.lk" <<'EOF'
$a = channel(1)
$b = channel(1)
parallel(function() { $v = receive($a); send($b, $v) })
print(receive($b))
EOF
run "the script and a task wait on each other" both_wait.lk 1 'deadlock'

cat > "$T/task_ends.lk" <<'EOF'
$ch = channel(1)
parallel(function() { $x = 1 })
print(receive($ch))
EOF
run "the only task ends without sending" task_ends.lk 1 'deadlock'

# Not deadlocks.
cat > "$T/timer.lk" <<'EOF'
$ch = channel(1)
timer::after(200, function() { send($ch, "from timer") })
print(receive($ch))
EOF
run "a timer can still send" timer.lk 0 '^from timer$'

cat > "$T/slow_task.lk" <<'EOF'
$ch = channel(0)
parallel(function() { $x = 0; for ($i = 0; $i < 3000000; $i++) { $x = $x + 1 }; send($ch, $x) })
print(receive($ch))
EOF
run "a task still computing is not a deadlock" slow_task.lk 0 '^3000000$'

cat > "$T/pipeline.lk" <<'EOF'
$a = channel(0)
$b = channel(0)
parallel(function() { for ($i = 1; $i <= 100; $i++) { send($a, $i) } })
parallel(function() { for ($i = 1; $i <= 100; $i++) { send($b, receive($a) * 2) } })
$s = 0
for ($i = 1; $i <= 100; $i++) { $s = $s + receive($b) }
print($s)
EOF
for r in 1 2 3; do run "unbuffered pipeline (run $r)" pipeline.lk 0 '^10100$'; done

# The web server: the same sizes (channel(0) unbuffered, negative an error).
cat > "$T/web.lk" <<'EOF'
route("GET", "/", function() {
  $ch = channel(0)
  parallel(function() { send($ch, 7) })
  print(receive($ch))
})
route("GET", "/neg", function() {
  try { $c = channel(-1); print("made") } catch ($e) { print("error") }
})
EOF
PORT=7797
(cd "$T" && LOOK_SECRET=chan-unbuffered-0123456789abcdef0123456 exec "$FCGI" --mode http --port $PORT web.lk >"$T/srv.log" 2>&1) & pid=$!
for i in $(seq 1 100); do curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done
w1=$(curl -s -m 10 "http://127.0.0.1:$PORT/"); w2=$(curl -s -m 10 "http://127.0.0.1:$PORT/neg")
[ "$w1" = 7 ] && [ "$w2" = error ] && echo "  OK   web: channel(0) works, a negative size is an error" || { echo "  FAIL web: [$w1] [$w2]"; tail -3 "$T/srv.log"; fail=1; }

[ $fail = 0 ] && echo "PASS: channel(0) is unbuffered; a deadlock is an error, not a hang" || { echo "FAIL: unbuffered channel"; exit 1; }
