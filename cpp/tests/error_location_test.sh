#!/usr/bin/env bash
# A run-time error names the file it happened in.
#
# An error inside a function of an included file (use "lib/x.lk") was reported with the MAIN
# script's name and the included file's line number — "File: main.lk, Line: 7" for a main.lk
# of five lines — sending the reader to the wrong file.
#   * an error in a function of an included file names that file and its line
#   * an error in the top-level code of an included file names that file
#   * an error in a nested include names the nested file
#   * an error in the main script still names the main script
# Usage: error_location_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"   # mutlak: her vaka geçici dizine cd yapar
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/lib"

printf '// kütüphane\n\n\n\nfunction boom() {\n    $x = 1\n    $y = $x / 0\n    return $y\n}\n' > "$TMP/lib/util.lk"
printf '// alt kütüphane\nfunction deep() {\n    return 1 / 0\n}\n' > "$TMP/lib/deep.lk"
printf 'use "deep.lk"\nfunction mid() {\n    return deep()\n}\n' > "$TMP/lib/mid.lk"
printf '// üst düzeyde hata\n$a = 1\n$b = $a / 0\n' > "$TMP/lib/top.lk"

printf 'use "lib/util.lk"\nprint("start")\nboom()\n' > "$TMP/fn.lk"
printf 'print("start")\nuse "lib/top.lk"\n' > "$TMP/top.lk"
printf 'use "lib/mid.lk"\nmid()\n' > "$TMP/nested.lk"
printf 'use "lib/util.lk"\n$q = 0\n\n$r = 5 / $q\n' > "$TMP/main.lk"

fail=0
check() {   # check TAG FILE WANT_FILE WANT_LINE
    local out; out="$(cd "$TMP" && "$LK" "$2" 2>&1)"
    local f l
    f="$(echo "$out" | sed -n 's/^  File: //p' | head -1)"
    l="$(echo "$out" | sed -n 's/^  Line: \([0-9]*\).*/\1/p' | head -1)"
    if [ "$f" = "$3" ] && [ "$l" = "$4" ]; then echo "  OK   $1: $f line $l"
    else echo "  FAIL $1: got '$f' line '$l', want '$3' line $4"; fail=1; fi
}
check "function in an included file"   fn.lk     lib/util.lk 7
check "top level of an included file"  top.lk    lib/top.lk  3
check "nested include"                 nested.lk lib/deep.lk 3
check "the main script"                main.lk   main.lk     4

[ $fail = 0 ] && echo "PASS: a run-time error names the file it happened in" || echo "FAIL: error location"
exit $fail
