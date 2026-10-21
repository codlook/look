#!/usr/bin/env bash
# A script that touched timer:: ends at once.
#
# The timer thread is stopped at exit. The stop flag used to be written without the timer
# lock: when it landed after the thread had checked the flag but before it went to sleep,
# the wake-up was lost, the thread slept to its deadline (30 seconds when idle) and the
# exit waited for it. Measured: up to 1 run in 30 hung.
#   * a script without timers ends at once
#   * a script that arms and cancels a timer ends at once
#   * a script that ends with a timer still set ends at once
# Usage: timer_exit_test.sh <lk> [runs]
LK="${1:?usage: $0 <lk> [runs]}"
RUNS="${2:-150}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
command -v timeout >/dev/null 2>&1 || { echo "  (skipped: timeout needed)"; exit 0; }

printf 'print("ok")\n' > "$TMP/plain.lk"
printf '$id = timer::after(60000, function() { print("late") })\ntimer::cancel($id)\nprint("ok")\n' > "$TMP/cancel.lk"
printf 'timer::after(60000, function() { print("late") })\nprint("ok")\n' > "$TMP/pending.lk"

fail=0
for case in plain cancel pending; do
    hung=0; wrong=0
    for i in $(seq "$RUNS"); do
        out="$(timeout 5 "$LK" "$TMP/$case.lk" 2>/dev/null)"; rc=$?
        if [ $rc -eq 124 ]; then hung=$((hung + 1)); continue; fi
        [ "${out##*$'\n'}" = "ok" ] || wrong=$((wrong + 1))
    done
    if [ $hung -eq 0 ] && [ $wrong -eq 0 ]; then echo "  OK   $case: $RUNS runs ended at once"
    else echo "  FAIL $case: $hung of $RUNS runs hung at exit, $wrong printed something else"; fail=1; fi
done

[ $fail = 0 ] && echo "PASS: scripts with timers end at once" || echo "FAIL: timer exit"
exit $fail
