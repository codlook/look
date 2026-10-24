#!/usr/bin/env bash
# Every thread variable says why it is safe with fibers.
#
# With fibers one worker thread runs many requests and switches between them whenever one waits
# (database, Redis, SMTP, HTTP, DNS). A thread_local written before such a wait and read after it
# can then belong to another request. Found twice in v2 when the network clients started to
# suspend: the session Redis connection was one per thread (a user received another user's
# session) and the HTTP client's connection-error text could come from another request.
# So each thread_local must carry "fiber-ok: <reason>" on its line or in the 8 lines above:
# per-fiber storage first (FiberLocal) with the thread variable only outside fibers, thread
# structure (scheduler, event loop), a cache with no wait between write and read, no fibers
# on that thread, or load time only. A new one without a reason fails here.
# Usage: thread_local_guard.sh   (run from anywhere)
cd "$(dirname "$0")/.." || exit 1
bad=""
n=0
while IFS= read -r f; do
    out=$(awk -v F="$f" '
        { L[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                s = L[i]
                if (s !~ /(^|[^a-zA-Z_])thread_local([^a-zA-Z_]|$)/) continue
                code = s; sub(/\/\/.*$/, "", code)
                if (code !~ /(^|[^a-zA-Z_])thread_local([^a-zA-Z_]|$)/) continue   # yalnız yorumda geçiyor
                ok = 0; for (k = i - 8; k <= i; k++) if (k > 0 && L[k] ~ /fiber-ok:/) ok = 1
                print (ok ? "OK " : "BAD ") F ":" i ": " s
            }
        }' "$f")
    [ -z "$out" ] && continue
    n=$((n + $(echo "$out" | wc -l)))
    bad="$bad$(echo "$out" | grep '^BAD ' | sed 's/^BAD //')"$'\n'
done < <(find src include -name '*.cpp' -o -name '*.h' | sort)
bad="$(echo "$bad" | sed '/^$/d')"
if [ -n "$bad" ]; then
    echo "FAIL: thread_local without a 'fiber-ok:' reason (a request can be suspended and another one run on the same thread):"
    echo "$bad" | sed -E 's/[ \t]+/ /g; s/^/    /'
    exit 1
fi
echo "PASS: all $n thread_local variables state why they are safe with fibers"
