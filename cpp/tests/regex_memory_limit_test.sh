#!/usr/bin/env bash
# string::regex works in a process with a limited virtual address space (shared hosting).
#
# 1.0.34 gave each of the 8 regex threads a 256 MB stack (a 64 KB input needs that much for some
# patterns) and started all 8 at the first regex: about 2 GB of address space. Under a
# `ulimit -v` of 1 GB, common on shared hosting (CloudLinux), some threads could not start and
# the process hung. The threads now start one at a time as needed; when one cannot start, its
# work waits for a running one, and only if none can start is there an error.
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
# 8 requests at once in lk-fcgi under 2 GB (the server itself needs more than 1 GB on a
# many-core machine): 8 regex threads of 256 MB plus the server do not fit
( cd "$TMP" && ulimit -v 2097152 && exec "$FCGI" --mode http --port "$PORT" --workers 8 web.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
cpids=""; for i in 1 2 3 4 5 6 7 8; do curl -s -m 20 "localhost:$PORT/rx" > "$TMP/r$i" & cpids="$cpids $!"; done; wait $cpids
got="$(for f in "$TMP"/r?; do cat "$f"; echo; done | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
alive="$(curl -s -m 3 "localhost:$PORT/ok")"
kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$got" = " 8 match;" ] && [ "$alive" = alive ]; then echo "  OK   lk-fcgi, ulimit -v 2 GB: 8 concurrent regex requests all answered"
else echo "  FAIL lk-fcgi, ulimit -v 2 GB: answers '$got', server '$alive'"; fail=1; fi
[ $fail = 0 ] && echo "PASS: regex works under a virtual memory limit" || { echo "FAIL: regex under a virtual memory limit"; exit 1; }
