#!/usr/bin/env bash
# A host function value (a Closure carrying a C++ function: lk test's assert_eq, test …) behaves
# like any function value where values travel — nothing crashes; it works or the error is clear:
#   * handed to another thread (parallel) or a timer: it runs there; when it throws, the error is
#     logged with its reason
#   * stored in the cache and called after reading it back: it still works
#   * json::encode gives null (as for any function), type::of says "function", == compares identity
# Usage: native_fn_boundary_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/tests"
cat > "$TMP/tests/test_native.lk" <<'LK'
use cache
use json
use type
test("handed to parallel", function() {
    parallel(assert_true)
    parallel::wait(2000)
    assert(true)
})
test("handed to a timer, called later with no arguments", function() {
    timer::after(10, test)
    timer::wait(2000)
    assert(true)
})
test("stored in the cache and called after reading it back", function() {
    cache::set("k", assert_eq, 60)
    $f = cache::get("k")
    $f(1, 1)
    assert_throws(function() use ($f) { $f(1, 2) })
})
test("json, type::of and ==", function() {
    assert_eq(json::encode(assert_eq), "null")
    assert_eq(type::of(assert_eq), "function")
    assert(assert_eq == assert_eq)
    assert(assert_eq != assert_true)
})
LK
out="$(cd "$TMP" && timeout 60 "$LK" test native 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tr -d '\r')"; rc=$?
fail=0
echo "$out" | grep -q '4/4 passed' || { echo "  FAIL not 4/4: [$out]"; fail=1; }
echo "$out" | grep -qF 'assert_true() failed — value is falsy' || { echo "  FAIL parallel error not logged with its reason"; fail=1; }
echo "$out" | grep -qF 'timer callback error: test() — expects test(name' || { echo "  FAIL timer error not logged with its reason"; fail=1; }
[ $fail = 0 ] && echo "  OK   parallel, timer, cache, json, type::of and == with a host function: no crash, errors named" \
              && echo "PASS: a host function value travels like any function value" || { echo "FAIL: host function boundaries"; exit 1; }
