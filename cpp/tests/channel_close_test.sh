#!/usr/bin/env bash
# Closing a channel wakes the senders waiting on it, and each gets "send on closed channel"
# (as in Go). close() used to wake only the receivers: a send() waiting on a full channel stayed
# blocked for ever.
# Usage: channel_close_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0

cat > "$T/a.lk" <<'EOF'
$ch = channel(1)
$res = channel(1)
parallel(function() {
  send($ch, 1)
  try { send($ch, 2); send($res, "sent") } catch ($e) { send($res, "error: " . $e) }
})
while (chan_size($ch) < 1) { $x = 1 }
close($ch)
print(receive($res))
print(receive($ch))
print(receive($ch))
EOF
for run in 1 2 3 4 5; do
  got=$(cd "$T" && timeout 10 "$LK" a.lk 2>&1); rc=$?
  want=$'error: send on closed channel\n1\nnull'
  if [ $rc = 0 ] && [ "$got" == "$want" ]; then echo "  OK   a waiting sender wakes with the error (run $run)"
  else echo "  FAIL run $run: rc=$rc [$got]"; fail=1; fi
done

cat > "$T/b.lk" <<'EOF'
$ch = channel(2)
close($ch)
try { send($ch, 1); print("sent") } catch ($e) { print("error: " . $e) }
EOF
got=$(cd "$T" && timeout 10 "$LK" b.lk 2>&1)
[ "$got" == "error: send on closed channel" ] && echo "  OK   send after close is the error" || { echo "  FAIL send after close: [$got]"; fail=1; }

[ $fail = 0 ] && echo "PASS: closing a channel wakes its senders with an error" || { echo "FAIL: channel close"; exit 1; }
