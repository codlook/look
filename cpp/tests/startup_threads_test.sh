#!/usr/bin/env bash
# A script starts no thread it does not use.
#
# The regex guard runs matches on a pool of 8 threads. The pool used to be created while the
# module table was built - in every process: each lk run and each lk-cgi request started 8
# threads, whether it used a regex or not (measured: 8 clone() calls per CGI request). The
# pool is now created by the first regex call.
#   * a script without regex runs on one thread
#   * a script that calls a regex has the pool (proves the count is read correctly)
# Linux only (reads /proc/<pid>/task).
# Usage: startup_threads_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
[ -d /proc/self/task ] || { echo "  (skipped: no /proc)"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# A busy loop keeps the script alive long enough to look at it.
LOOP='$i = 0
while ($i < 20000000) { $i = $i + 1 }
print("done")'
printf '%s\n' "$LOOP" > "$TMP/plain.lk"
printf 'use string\nprint(string::regex_match("abc", "b"))\n%s\n' "$LOOP" > "$TMP/regex.lk"

threads() {   # threads FILE -> the largest thread count seen while it runs
    # Sampled until the process ends, not once after a fixed wait: a single look could come
    # before the threads start (slow machine, sanitizer build) and miss them.
    "$LK" "$1" > /dev/null 2>&1 & local p=$!
    local max=0 n
    while kill -0 "$p" 2>/dev/null; do
        n=$(ls /proc/$p/task 2>/dev/null | wc -l)
        [ "$n" -gt "$max" ] && max=$n
        sleep 0.01
    done
    wait $p
    echo "$max"
}

fail=0
n=$(threads "$TMP/regex.lk")
if [ "$n" -gt 1 ]; then echo "  OK   a script that calls a regex has the pool ($n threads)"
else echo "  FAIL a script that calls a regex shows $n thread(s) — the test cannot see threads (loop too short?)"; fail=1; fi
n=$(threads "$TMP/plain.lk")
if [ "$n" = 1 ]; then echo "  OK   a script without regex runs on one thread"
else echo "  FAIL a script without regex runs on $n threads (expected 1)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: a script starts no thread it does not use" || echo "FAIL: startup threads"
exit $fail
