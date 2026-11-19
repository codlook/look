#!/usr/bin/env bash
# test_runner_test.sh — verifies the correctness of the `lk test` framework itself.
#
# Bug 50: assert_throws()/assert::throws() used to return a marker array and
# expect the runner to find it in the test's RETURN value. But LOOK does not
# implicitly RETURN the last expression → the marker was always dropped → assert_throws() never
# asserted. Even the documented usage (assert_throws as the last expression) let an error that
# should be caught slip by silently, and the test reported a FALSE GREEN.
#
# This guard verifies that functions that SHOULD throw but DON'T are caught by assert_throws
# (the test FAILS). With the old broken code
# these tests passed GREEN → the guard catches this regression (positive control).
set -u
LK="${1:-./build/lk}"
# convert to an absolute path (we cd into the test directory below)
case "$LK" in /*) ;; *) LK="$(pwd)/$LK" ;; esac
fail=0
work="$(mktemp -d)"; mkdir -p "$work/tests"
trap 'rm -rf "$work"' EXIT

# Expectation: assert_throws must FAIL the test if fn does NOT throw.
cat > "$work/tests/t.lk" <<'EOF'
test("son ifade, fn firlatmiyor", function() {
    assert_throws(function() { $x = 1 })
})
test("son DEGIL, fn firlatmiyor", function() {
    assert_throws(function() { $x = 1 })
    $y = 2
})
use assert;
test("modul formu, fn firlatmiyor", function() {
    assert::throws(function() { $x = 1 })
})
test("dogru kullanim, fn firlatiyor => GECMELI", function() {
    assert_throws(function() { throw "beklenen" })
    $z = 3
})
EOF

out="$(cd "$work" && "$LK" test 2>&1 || true)"
# Strip color codes
plain="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')"

# expect 3 failed + 1 of 4 passed
if ! printf '%s' "$plain" | grep -q "3 başarısız"; then
    echo "FAIL: assert_throws firlatMAYAN fn'i yakalamiyor (3 basarisiz bekleniyordu)"
    printf '%s\n' "$plain" | grep -E "❌|✅|geçti|başarısız"
    fail=1
fi
if ! printf '%s' "$plain" | grep -q "1/4 geçti"; then
    echo "FAIL: dogru assert_throws kullanimi (fn firlatiyor) GECMEDI (1/4 bekleniyordu)"
    printf '%s\n' "$plain" | grep -E "❌|✅|geçti|başarısız"
    fail=1
fi

if [ $fail = 0 ]; then
    echo "PASS: assert_throws/assert::throws — konumdan bagimsiz, HEMEN calisip firlatmayi dogruluyor (50. bug)"
fi
exit $fail
