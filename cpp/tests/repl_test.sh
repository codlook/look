#!/usr/bin/env bash
# `lk repl` — the interactive session, driven through stdin.
#   * variables and functions survive from one line to the next
#   * the value of the last expression is shown (=> …), an assignment shows the assigned value,
#     and a line that prints shows its output instead
#   * a pure built-in typed alone shows its value (its result is used: it is displayed)
#   * standard modules are open without `use`; a module a line opens with `use` works
#   * a misspelled module function, an undefined function and a run-time error are reported and
#     the session goes on, keeping what was set before
#   * :vars lists variables as $name and functions as name()
#   * LOOK 2: lines run on the VM — a rule only the VM applies (a discarded pure result inside a
#     line) is reported
# Usage: repl_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
cat > "$TMP/in.txt" <<'LK'
$x = 5
$x * 2
function sq($n) { return $n * $n }
sq($x)
strtoupper("abc")
print("hi")
use cache
cache::set("k", 7, 60)
cache::get("k")
string::uper("a")
nosuch()
$y = 1 / 0
$x
$q = [1]; array::push($q, 2); $z = 1
:vars
:exit
LK
out="$(cd "$TMP" && "$LK" repl < "$TMP/in.txt" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tr -d '\r')"
has() { echo "$out" | grep -qF -- "$1"; }
has '=> 5' && has '=> 10' && has '=> 25' && ok "variables and functions survive between lines; values are shown" || bad "state: [$out]"
has '=> "ABC"' && ok "a pure built-in typed alone shows its value" || bad "strtoupper: [$out]"
has '>>> hi' && ok "a line that prints shows its output" || bad "print: [$out]"
has '=> 7' && ok "a module opened with use on a line works" || bad "use cache: [$out]"
has 'string::uper() does not exist' && has 'Undefined variable: nosuch' && has 'Division by zero' \
    && ok "errors are reported and the session goes on" || bad "errors: [$out]"
[ "$(echo "$out" | grep -c '=> 5')" = 2 ] && ok "a value set before an error is kept" || bad "kept: [$out]"
has 'line 1: the result of array::push() is not used' && ok "lines run on the VM (a discarded pure result inside a line is reported)" \
                                                   || bad "vm rule: [$out]"
if echo "$out" | grep -qE '(^|>>> )  [$]x$' && echo "$out" | grep -qx '  sq()' && ! echo "$out" | grep -qF '$$'; then   # TTY yok: istem aynı satırda
    ok ":vars lists \$x and sq()"
else bad ":vars: [$out]"; fi
has 'See you!' && ok ":exit ends the session" || bad "exit: [$out]"

[ $fail = 0 ] && echo "PASS: lk repl" || { echo "FAIL: lk repl"; exit 1; }
