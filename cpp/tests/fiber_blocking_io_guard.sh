#!/usr/bin/env bash
# The network clients do not block the worker thread while a fiber waits.
#
# A web request runs in a fiber; while it waits for the database, Redis, an SMTP server or an
# HTTP server, only the fiber may wait — the worker thread must serve other requests. Two forms
# of the same mistake were found:
#   * a poll with a timeout before suspending: fiber_wait_readable polled 1 ms first (every
#     database wait held the thread 1 ms; the database route ran ~15% behind, off-CPU profile);
#   * a plain recv/SSL_read with no fiber wait before it: the Redis client and mail::send held
#     the thread for the whole reply; connecting to MySQL/PostgreSQL polled up to 5 s.
# Timing tests miss this (1 ms is noise), so this guard reads the code. In the client files:
#   1. a poll() with a timeout other than 0 must carry "blocking-ok" on its line or the 2 before;
#   2. a recv()/SSL_read() must not block: MSG_DONTWAIT (same or next line), or a fiber wait
#      (fiber_wait_readable, client_wait_readable, wait_readable_tmo) in the 8 lines before, or
#      "blocking-ok" in the 3 lines before.
# "blocking-ok" says why blocking is right there (no fiber: command line, setup). Windows
# branches (#ifdef _WIN32 … #else/#endif) are skipped: there are no fibers on Windows.
# Usage: fiber_blocking_io_guard.sh   (run from anywhere)
cd "$(dirname "$0")/.." || exit 1
files="include/look/fiber_io.h src/mysql_client.cpp src/postgres_client.cpp src/resp_client.cpp src/mail_stdlib.cpp src/http_client.cpp"
check_file() {
    awk -v F="$1" '
    { L[NR] = $0 }
    END {
        depth = 0; win = 0
        for (i = 1; i <= NR; i++) {
            s = L[i]
            if (s ~ /^[ \t]*#[ \t]*if/) {
                depth++
                if (s ~ /#[ \t]*ifdef[ \t]+_WIN32/ || s ~ /#[ \t]*if[ \t]+defined\(_WIN32\)/) win = depth
                continue
            }
            if (s ~ /^[ \t]*#[ \t]*else/ || s ~ /^[ \t]*#[ \t]*elif/) { if (win == depth) win = -depth; continue }
            if (s ~ /^[ \t]*#[ \t]*endif/) { if (win == depth || win == -depth) win = 0; depth--; continue }
            if (win > 0) continue                                  # Windows dalı
            if (s ~ /^[ \t]*\/\//) continue                        # yorum satırı
            code = s; sub(/[ \t]*\/\/.*$/, "", code)                # satır sonu yorumu
            mark2 = 0; for (k = i - 2; k <= i; k++) if (k > 0 && L[k] ~ /blocking-ok/) mark2 = 1
            mark3 = 0; for (k = i - 3; k <= i; k++) if (k > 0 && L[k] ~ /blocking-ok/) mark3 = 1
            if (code ~ /(^|[^a-zA-Z_])poll[ \t]*\(/) {
                t = code; sub(/.*poll[ \t]*\(/, "", t)
                split(t, a, ","); tmo = a[3]; sub(/^[ \t]+/, "", tmo); sub(/[ \t)].*$/, "", tmo)
                if (tmo != "0" && !mark2) print F ":" i ": poll with timeout " tmo ": " s
            }
            if (code ~ /(^|[^a-zA-Z_])(recv|SSL_read)[ \t]*\(/ && code !~ /^(static )?int resp_recv/) {
                nb = (code ~ /MSG_DONTWAIT/ || L[i + 1] ~ /MSG_DONTWAIT/)
                waited = 0; for (k = i - 8; k < i; k++) if (k > 0 && L[k] ~ /fiber_wait_readable|client_wait_readable|wait_readable_tmo/) waited = 1
                if (!nb && !waited && !mark3) print F ":" i ": read without a fiber wait: " s
            }
        }
    }' "$1"
}
bad=""
for f in $files; do bad="$bad$(check_file "$f")"$'\n'; done
bad="$(echo "$bad" | sed '/^$/d')"
if [ -n "$bad" ]; then
    echo "FAIL: a network client can block the worker thread inside a fiber:"
    echo "$bad" | sed -E 's/[ \t]+/ /g; s/^/    /'
    exit 1
fi
echo "PASS: fiber waits in the network clients never block the worker thread ($(echo $files | wc -w) files)"
