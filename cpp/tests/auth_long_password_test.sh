#!/usr/bin/env bash
# auth::verify takes the same time for a long password as for a short one, and still matches
# hashes made by a standard PBKDF2-HMAC-SHA256 (Python hashlib) for every length.
#
# The HMAC inside PBKDF2 hashed a key longer than 64 bytes again in every round, so the cost was
# password length x rounds: a 100 KB password took 3.1 s at 10,000 rounds (16 bytes: 55 ms), a 1 MB
# one about 5 minutes at the default 100,000 — a few login requests with a long password held the
# whole pool (Django CVE-2013-1443 class; checked while looking at the OpenSSH musl long-password
# bypass, CERTFR-2026-ALE-015). The long key is now hashed once; the result is the same (RFC 2104).
# Usage: auth_long_password_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
for n in 0 1 64 65 1000 100000; do
    python3 - "$n" > "$TMP/h.txt" <<'PY'
import hashlib, base64, os, sys
n = int(sys.argv[1]); pwd = (b"p" * n); salt = os.urandom(32)
dk = hashlib.pbkdf2_hmac("sha256", pwd, salt, 10000, 32)
print("pbkdf2$sha256$10000$" + base64.b64encode(salt).decode() + "$" + base64.b64encode(dk).decode())
PY
    h="$(cat "$TMP/h.txt")"
    printf 'use auth\nuse string\n$p = string::repeat("p", %s)\n$a = auth::verify($p, "%s") ? "match" : "no"\n$b = auth::verify($p . "x", "%s") ? "match" : "no"\nprint($a . " " . $b)\n' "$n" "$h" "$h" > "$TMP/v.lk"
    s=$(date +%s%N); got="$("$LK" "$TMP/v.lk" 2>&1 | tail -1)"; ms=$(( ($(date +%s%N) - s) / 1000000 ))
    [ "$got" = "match no" ] || { echo "  FAIL length $n: '$got' (want 'match no')"; fail=1; continue; }
    echo "  OK   length $n: matches the standard hash, a wrong password does not (${ms} ms for two checks)"
    [ "$n" = 100000 ] && { [ $ms -lt 1500 ] && echo "  OK   a 100 KB password costs about what a short one does" \
        || { echo "  FAIL a 100 KB password took ${ms} ms"; fail=1; }; }
done
[ $fail = 0 ] && echo "PASS: auth::verify is standard PBKDF2 and its cost does not grow with the password" \
             || { echo "FAIL: auth::verify with long passwords"; exit 1; }
