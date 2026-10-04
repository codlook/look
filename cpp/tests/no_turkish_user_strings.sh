#!/usr/bin/env bash
# Everything the runtime shows a user is English: error messages, log lines, CLI output,
# JSON keys. A Turkish message cannot be found in the English documentation or searched for.
# Comments stay Turkish (internal notes), so this looks only inside string literals, after
# stripping // comments, for the letters that exist in Turkish and not in English.
# It cannot see Turkish written with plain ASCII letters ("hata", "kuruldu") — those were
# removed by hand; this rule stops the obvious ones from coming back.
# Usage: no_turkish_user_strings.sh   (run from cpp/)
cd "$(dirname "$0")/.." || exit 1
fail=0
for f in src/*.cpp include/look/*.h; do
    bad="$(sed 's://.*$::' "$f" | LC_ALL=C.UTF-8 grep -nE '"[^"]*(ç|ğ|ı|ö|ş|ü|Ç|Ğ|İ|Ö|Ş|Ü)[^"]*"' || true)"
    if [ -n "$bad" ]; then echo "FAIL: Turkish text in a string literal in $f:"; echo "$bad" | cut -c1-160 | sed 's/^/    /'; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: no Turkish text in runtime string literals" || exit 1
