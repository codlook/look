#!/usr/bin/env bash
# `lk test` — the test runner does what the documentation says.
#   * test("name", fn) registers a test; a passing and a failing test are counted, the summary
#     line says how many failed, and the exit status is 1 when any failed (0 when none did)
#   * every assert_*() passes on a true case and fails with its own message on a false one
#   * assert_throws(fn) passes when fn throws and fails when it does not
#   * before_each / after_each run around every test, after_each also when the test failed
#   * `use assert` and assert::eq(...) work like assert_eq(...)
#   * each test starts from the file's top-level state: a global a test changes is back to its
#     top-level value in the next test
#   * `lk test name` runs tests/test_name.lk; functions from `use "lib.lk"` are available
#   * a top-level error and a parse error are reported as such, not as a passing run
#   * LOOK 2: the test file runs on the VM — a rule only the VM applies stops it at load, and a
#     misspelled assert::… function is named
# Usage: lk_test_runner_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
plain() { sed 's/\x1b\[[0-9;]*m//g' | tr -d '\r'; }
mkdir -p "$TMP/p/tests"

cat > "$TMP/p/tests/test_main.lk" <<'LK'
use "lib.lk"
use assert
$g = 1
test("passes", function() {
    assert(true)
    assert_true(1)
    assert_false(0)
    assert_eq(double(2), 4)
    assert_neq(1, 2)
    assert_null(null)
    assert_not_null(0)
    assert_contains([1, 2, 3], 2)
    assert_match("abc123", "[0-9]+")
    assert_throws(function() { throw "boom" })
    assert::eq(double(3), 6)
})
test("fails on assert_eq", function() {
    assert_eq(double(2), 5)
})
test("changes a global", function() {
    $g = 2
    assert_eq($g, 2)
})
test("sees the top-level value again", function() {
    assert_eq($g, 1)
})
test("assert_throws on a call that does not throw fails", function() {
    assert_throws(function() { $x = 1 })
})
LK
printf 'function double($n) { return $n * 2 }\n' > "$TMP/p/tests/lib.lk"
out="$(cd "$TMP/p" && "$LK" test main 2>&1 | plain)"; rc="$(cd "$TMP/p" && "$LK" test main >/dev/null 2>&1; echo $?)"
echo "$out" | grep -q '✅ passes' && ok "a test with every assert_*(), assert::eq and functions from use \"lib.lk\" passes" \
                                  || bad "passes: [$out]"
if echo "$out" | grep -q '❌ fails on assert_eq' && echo "$out" | grep -q 'expected: 5' && echo "$out" | grep -q 'actual:   4'; then
    ok "a failing assert_eq is reported with expected and actual"
else bad "assert_eq failure: [$out]"; fi
echo "$out" | grep -q '✅ sees the top-level value again' && ok "each test starts from the top-level state (a global changed by a test is back)" \
                                                          || bad "isolation: [$out]"
echo "$out" | grep -q '❌ assert_throws on a call that does not throw fails' && ok "assert_throws fails when the call does not throw" \
                                                                             || bad "assert_throws: [$out]"
if echo "$out" | grep -q '2 failed — 3/5 passed' && [ "$rc" = 1 ]; then ok "the summary counts 2 failed of 5, exit status 1"
else bad "summary: rc=$rc [$(echo "$out" | tail -2)]"; fi

# before_each / after_each order, after_each also after a failure
cat > "$TMP/p/tests/test_hooks.lk" <<'LK'
use file
before_each(function() { file::append("order.txt", "B ") })
after_each(function() { file::append("order.txt", "A ") })
test("one", function() { file::append("order.txt", "1 ") })
test("two fails", function() { file::append("order.txt", "2 "); assert(false) })
LK
rm -f "$TMP/p/order.txt"
(cd "$TMP/p" && "$LK" test hooks >/dev/null 2>&1)
order="$(tr -d '\r' < "$TMP/p/order.txt" 2>/dev/null)"
[ "$order" = "B 1 A B 2 A " ] && ok "before_each / after_each run around every test, after_each after a failure too" \
                              || bad "hooks order: [$order]"

# all passing → exit 0
printf 'test("only", function() { assert_eq(1, 1) })\n' > "$TMP/p/tests/test_green.lk"
out="$(cd "$TMP/p" && "$LK" test green 2>&1 | plain)"; rc="$(cd "$TMP/p" && "$LK" test green >/dev/null 2>&1; echo $?)"
echo "$out" | grep -q '1/1 passed' && [ "$rc" = 0 ] && ok "all passing: 1/1 passed, exit status 0" || bad "green: rc=$rc [$out]"

# a top-level error and a parse error
printf 'throw "setup broke"\ntest("never", function() { assert(true) })\n' > "$TMP/p/tests/test_setup.lk"
out="$(cd "$TMP/p" && "$LK" test setup 2>&1 | plain)"
echo "$out" | grep -q '(setup error)' && echo "$out" | grep -q 'setup broke' && ok "a top-level error is reported as a setup error" \
                                                                            || bad "setup error: [$out]"
printf 'test("x", function() { assert(true) \n' > "$TMP/p/tests/test_parse.lk"
out="$(cd "$TMP/p" && "$LK" test parse 2>&1 | plain)"
echo "$out" | grep -q '(parse error)' && ok "a parse error is reported as a parse error" || bad "parse error: [$out]"

# the test file runs on the VM: a rule only the VM applies (a discarded pure result) stops it at
# load, and a misspelled assert function is named
printf 'use array\n$a = [1]\narray::push($a, 2)\ntest("never", function() { assert(true) })\n' > "$TMP/p/tests/test_vm.lk"
out="$(cd "$TMP/p" && "$LK" test vm 2>&1 | plain)"
if echo "$out" | grep -q '(load error)' && echo "$out" | grep -q 'line 3: the result of array::push() is not used'; then
    ok "the test file runs on the VM (a discarded array::push stops it at load)"
else bad "vm: [$out]"; fi
printf 'use assert\ntest("typo", function() { assert::eqq(1, 1) })\n' > "$TMP/p/tests/test_typo.lk"
out="$(cd "$TMP/p" && "$LK" test typo 2>&1 | plain)"
echo "$out" | grep -q 'assert::eqq() does not exist' && ok "a misspelled assert::eqq is named before anything runs" || bad "typo: [$out]"

[ $fail = 0 ] && echo "PASS: lk test does what the documentation says" || { echo "FAIL: lk test"; exit 1; }
