#!/usr/bin/env bash
# Every built-in has an effect class (src/builtins.cpp: builtin_effect) — Pure, Reads, Effect,
# Callback or ThrowCheck. The rule for a discarded result (array::push alone on a line) applies only
# to Pure built-ins, so a built-in added without a class must not slip through unnoticed: this test
# fails on any unclassified built-in, and on a classified name that is not a built-in (a typo).
# Usage: builtin_effect_table_test.sh   (run from cpp/; needs g++)
set -u
cd "$(dirname "$0")/.."
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-g++}"
if ! "$CXX" -std=c++17 -Iinclude tests/builtin_effect_table_check.cpp src/builtins.cpp -o "$TMP/check" 2> "$TMP/err.txt"; then
    cat "$TMP/err.txt"; echo "FAIL: the check does not compile"; exit 1
fi
if "$TMP/check"; then echo "PASS: every built-in has an effect class"
else echo "FAIL: add the built-in to effect_rows() in src/builtins.cpp"; exit 1; fi
