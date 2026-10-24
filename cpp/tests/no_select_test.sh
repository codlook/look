#!/usr/bin/env bash
# select() / FD_SET are not used outside Windows-only code.
#
# An fd_set holds descriptors below 1024 only; FD_SET on a larger one writes outside it.
# The server keeps thousands of connections open, so a socket opened later (a database
# connection, for instance) gets a number above 1024. Up to 1.0.14 the MySQL and PostgreSQL
# connect timeouts and the IMAP idle wait used select(): measured under ASan/UBSan with 2000
# open WebSockets, `db::connect` wrote out of bounds (mysql_client.cpp, FD_SET). They use
# poll() now. Winsock's fd_set is a list of sockets, not a bitmap, so Windows code may keep it.
# Usage: no_select_test.sh   (run from cpp/)
fail=0
for f in src/*.cpp include/look/*.h; do
    awk -v F="$f" '
        # Windows dalı: #ifdef _WIN32 / #if defined(_WIN32) gövdesi ve #ifndef _WIN32 / #if !defined(_WIN32) bloğunun #else dalı.
        /^[ \t]*#[ \t]*if/    { depth++; neg[depth] = ($0 ~ /_WIN32/ && $0 ~ /!|ifndef/) ? 1 : 0; win[depth] = ($0 ~ /_WIN32/ && !neg[depth]) ? 1 : 0; next }
        /^[ \t]*#[ \t]*(else|elif)/ { win[depth] = neg[depth]; next }
        /^[ \t]*#[ \t]*endif/ { depth--; next }
        /^[ \t]*\/\//         { next }
        /FD_SET[ \t]*\(|[^a-zA-Z_:]select[ \t]*\(|::select[ \t]*\(/ {
            w = 0; for (i = 1; i <= depth; i++) if (win[i]) w = 1
            if (!w) { printf "  FAIL %s:%d: %s\n", F, NR, $0; bad = 1 }
        }
        END { exit bad }' "$f" || fail=1
done
[ $fail = 0 ] && echo "PASS: no select()/FD_SET outside Windows-only code" || { echo "FAIL: select()/FD_SET would overflow for descriptors above 1023 — use poll()"; exit 1; }
