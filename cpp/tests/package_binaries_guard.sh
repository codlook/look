#!/usr/bin/env bash
# Every LOOK binary the documentation names is in the release packages.
#
# The documentation describes running LOOK as CGI (lk-cgi; lk-cgi.exe on Windows/XAMPP), and the
# release build makes lk-cgi, but no package shipped it: the Linux and Plesk zips had lk and
# lk-fcgi only, the Windows zip lk.exe and lk-fcgi.exe. CGI users could only build from source —
# and the session security fixes of 1.0.38–1.0.40, which matter most for CGI on shared hosting,
# did not reach them. Now: each binary named in the docs must be in the package for its platform.
# Usage: package_binaries_guard.sh <linux.zip> <plesk.zip> [<windows.zip>]   (needs unzip)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LINUX="$1"; PLESK="$2"; WIN="${3:-}"
docs="$ROOT/README.md $ROOT/docs/look.codlook.com/docs.html $ROOT/platforms/linux/ubuntu/README.md $(ls "$ROOT"/platforms/plesk/*.md "$ROOT"/docs/*.md 2>/dev/null)"
named=$(cat $docs 2>/dev/null | grep -ohE '\blk(-fcgi|-cgi)(\.exe)?\b' | sort -u)
unix="lk $(echo "$named" | grep -v '\.exe$' | tr '\n' ' ')"
win="lk.exe $(echo "$named" | grep '\.exe$' | tr '\n' ' ')"
fail=0
has() {   # has <zip> <regex> — zip içinde yolu regex'e uyan bir dosya var mı
    unzip -Z1 "$1" 2>/dev/null | grep -qE "$2"
}
for b in $unix; do
    has "$LINUX" "(^|/)bin/$b\$"       && echo "  OK   linux zip has bin/$b"       || { echo "  FAIL linux zip has no bin/$b (named in the docs)"; fail=1; }
    has "$PLESK" "^htdocs/bin/$b\$"    && echo "  OK   plesk zip has htdocs/bin/$b" || { echo "  FAIL plesk zip has no htdocs/bin/$b (named in the docs)"; fail=1; }
done
if [ -n "$WIN" ]; then
    for b in $win; do
        has "$WIN" "(^|/)${b//./\\.}\$" && echo "  OK   windows zip has $b" || { echo "  FAIL windows zip has no $b (named in the docs)"; fail=1; }
    done
fi
[ $fail = 0 ] && echo "PASS: every binary the documentation names is in the packages" \
              || echo "FAIL: a binary the documentation names is missing from a package"
exit $fail
