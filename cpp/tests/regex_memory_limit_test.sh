#!/usr/bin/env bash
# string::regex works in a process with a limited virtual address space (shared hosting).
#
# 1.0.34 gave each of the 8 regex threads a 256 MB stack (a 64 KB input needs that much for some
# patterns) and started all 8 at the first regex: about 2 GB of address space. Under a
# `ulimit -v` of 1 GB, common on shared hosting (CloudLinux), some threads could not start and
# the process hung. The threads now start one at a time as needed; when one cannot start, its
# work waits for a running one, and only if none can start is there an error.
# Needs prlimit (util-linux).
# Usage: regex_memory_limit_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7751}"
cat > "$TMP/t.lk" <<'LK'
use string
$s = string::repeat("a", 65536)
print("short " . string::regex_match("abc", "^[a-z]+$"))
print("long " . string::regex_match($s, "^(a|b)*$"))
LK
cat > "$TMP/web.lk" <<'LK'
use string
route("GET", "/ok", fn() => response::text("alive"))
route("GET", "/rx", fn() => response::text(string::regex_match(string::repeat("a", 8192), "^(a|b)*$") ? "match" : "no"))
LK
fail=0
for kb in 1048576 524288; do   # 1 GB, 512 MB
    out="$( (ulimit -v $kb; timeout 20 "$LK" "$TMP/t.lk") 2>&1 )"; rc=$?
    if [ "$rc" = 0 ] && [ "$out" = $'short true\nlong true' ]; then
        echo "  OK   lk, ulimit -v $((kb / 1024)) MB: short and 64 KB matches work"
    else
        echo "  FAIL lk, ulimit -v $((kb / 1024)) MB: exit $rc$( [ $rc = 124 ] && echo ' (hung)') $(echo "$out" | tr '\n' ' ' | head -c 200)"; fail=1
    fi
done
# 8 requests at once in lk-fcgi with a limited address space: once the
# server is up, its address space is limited to its own size + 1 GB (prlimit). (A fixed limit
# would depend on the machine: the server's own size grows with the number of CPUs.)
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 8 web.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
vsz_kb="$(awk '/^VmSize/ {print $2}' "/proc/$pid/status")"
prlimit --pid "$pid" --as=$(( (vsz_kb + 1048576) * 1024 ))
cpids=""; for i in 1 2 3 4 5 6 7 8; do curl -s -m 20 -w ' %{http_code}' "localhost:$PORT/rx" > "$TMP/r$i" & cpids="$cpids $!"; done; wait $cpids
got="$(for f in "$TMP"/r?; do cat "$f"; echo; done | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
alive="$(curl -s -m 3 "localhost:$PORT/ok")"
kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$got" = " 8 match 200;" ] && [ "$alive" = alive ]; then echo "  OK   lk-fcgi, own size + 1 GB: 8 concurrent regex requests all answered"
else echo "  FAIL lk-fcgi, own size + 1 GB: answers '$got', server '$alive'"
     grep -E "ERROR" "$TMP/log.txt" | sort | uniq -c | head -5; fail=1; fi
[ $fail = 0 ] && echo "PASS: regex works under a virtual memory limit" || { echo "FAIL: regex under a virtual memory limit"; exit 1; }
