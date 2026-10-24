#!/usr/bin/env bash
# string::regex_* accept up to 64 KB of input; a long input no longer kills the process.
#
# std::regex matches recursively, one level per input character. The regex worker threads had
# the default 8 MB stack, so an input of ~10-20 KB overflowed it and the whole process died with
# SIGSEGV — in lk-fcgi that was every site on the server, from one form field. The workers now
# get a stack sized for the 64 KB limit.
# Usage: regex_long_input_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/t.lk" <<'LK'
use string
$s = string::repeat("a", 65536)
print("match " . string::regex_match($s, "^[a-z]+$"))
print("alt " . string::regex_match($s, "^(a|b)*$"))
print("any " . string::regex_match($s, "^(.|\n)*$"))
print("replace " . len(string::regex_replace($s, "^(a)+$", "x")))
print("all " . len(string::regex_match_all(string::repeat("ab", 32768), "(a|b)")))
print("done")
LK
out="$("$LK" "$TMP/t.lk" 2>&1)"; rc=$?
expect=$'match true\nalt true\nany true\nreplace 1\nall 65536\ndone'
if [ "$rc" = 0 ] && [ "$out" = "$expect" ]; then
    echo "PASS: 64 KB regex inputs are matched and the process survives"; exit 0
fi
echo "FAIL: exit $rc"; echo "--- expected"; echo "$expect"; echo "--- got"; echo "$out" | head -20; exit 1
