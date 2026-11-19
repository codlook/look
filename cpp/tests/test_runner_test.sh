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
test("last statement, fn does not throw", function() {
    assert_throws(function() { $x = 1 })
})
test("NOT the last statement, fn does not throw", function() {
    assert_throws(function() { $x = 1 })
    $y = 2
})
use assert;
test("module form, fn does not throw", function() {
    assert::throws(function() { $x = 1 })
})
test("correct use, fn throws => MUST PASS", function() {
    assert_throws(function() { throw "expected" })
    $z = 3
})
EOF

out="$(cd "$work" && "$LK" test 2>&1 || true)"
# Strip color codes
plain="$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')"

# expect 3 failed + 1 of 4 passed
if ! printf '%s' "$plain" | grep -q "3 failed"; then
    echo "FAIL: assert_throws does not catch a fn that does NOT throw (expected 3 failed)"
    printf '%s\n' "$plain" | grep -E "❌|✅|passed|failed"
    fail=1
fi
if ! printf '%s' "$plain" | grep -q "1/4 passed"; then
    echo "FAIL: correct assert_throws use (fn throws) did NOT pass (expected 1/4 passed)"
    printf '%s\n' "$plain" | grep -E "❌|✅|passed|failed"
    fail=1
fi

if [ $fail = 0 ]; then
    echo "PASS: assert_throws/assert::throws runs at once and checks the throw wherever it stands (bug 50)"
fi
exit $fail
