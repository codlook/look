#!/bin/bash
# Inside a function, one name cannot be both the outer variable (read) and a new local (assigned):
# `$n += 1` or `$n = $n + 1` looks like it bumps the outer counter, but it makes a local and the
# outer $n never changes — silently. As Python's UnboundLocalError, the order does not matter:
# read then assign, assign (in a block that ended) then read, in different branches, or in a loop
# whose first turn reads the outer value. A plain `$n = 5` with no read stays a local.
# Checked in a named function, a closure and a function from `use "file.lk"`.
# Usage: outer_read_assign_test.sh <lk> [<lk-fcgi>]
LK=${1:?usage: outer_read_assign_test.sh <lk> [<lk-fcgi>]}; FCGI=$2
LK=$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")
[ -n "$FCGI" ] && FCGI=$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; cd "$T"
fail=0; n=0
HDR='use array
$n = 1
$w = "a"
$a = [1]
$x = true'
SHOW='print($n . " " . $w . " " . json::encode($a))'

# name | body | the name in the message | R = read first, then assigned; B = assigned in a block that
# ended, then read (there the name is the outer variable)
REFUSED=(
  'plus_eq|$n += 1|n|R'
  'concat_eq|$w .= "z"|w|R'
  'self_add|$n = $n + 1|n|R'
  'read_then_assign|print($n)
  $n = 3|n|R'
  'push_assign|$a = array::push($a, 2)|a|R'
  'loop_read_first|$k = 0
  while ($k < 2) { print($n)
    $n = 1
    $k = $k + 1 }|n|R'
  'branches_assign_first|if ($x) { $n = 5 } else { print($n) }|n|B'
  'branches_read_first|if ($x) { print($n) } else { $n = 5 }|n|R'
  'after_block|if ($x) { $n = 5 }
  print($n)|n|B'
  'after_for|for ($n = 0; $n < 2; $n = $n + 1) { }
  print($n)|n|B'
)
# must load and leave the outer variables as declared
ALLOWED=(
  'plain_assign|$n = 100'
  'local_then_update|$t = 0
  $t += 1
  $t = $t + 1
  $t .= "x"'
  'loop_var|for ($i = 0; $i < 3; $i++) { $s = $i }
  foreach ([1, 2] as $v) { $q = $v + 1 }'
  'counter_loop|$c = 0
  while ($c < 3) { $c = $c + 1 }'
  'read_only|$r = $n + count($a)'
)

make() {  # make <ctx> <name> <body> → file name
  case $1 in
    fn)  printf '%s\nfunction f() {\n  %s\n}\nf()\n%s\n' "$HDR" "$3" "$SHOW" > "fn_$2.lk"; echo "fn_$2.lk" ;;
    cl)  printf '%s\n$c = function() {\n  %s\n}\n$c()\n%s\n' "$HDR" "$3" "$SHOW" > "cl_$2.lk"; echo "cl_$2.lk" ;;
    use) printf 'use array\nfunction g() {\n  %s\n}\n' "$3" > "lib_$2.lk"
         printf '%s\nuse "lib_%s.lk"\ng()\n%s\n' "$HDR" "$2" "$SHOW" > "use_$2.lk"; echo "use_$2.lk" ;;
  esac
}
who() { case $1 in fn) echo 'function f()';; cl) echo 'closure';; use) echo 'function g()';; esac; }

# (entries span lines: split on '|' with parameter expansion — `read` would stop at the first line)
for c in "${REFUSED[@]}"; do
  name=${c%%|*}; rest=${c#*|}; kind=${rest##*|}; rest=${rest%|*}; var=${rest##*|}; body=${rest%|*}
  for ctx in fn cl use; do
    n=$((n+1)); f=$(make $ctx "$name" "$body")
    out=$("$LK" "$f" 2>&1); rc=$?
    if [ "$kind" = R ]; then
      want="$(who $ctx) reads \$$var from outside it and also assigns \$$var"
      tail="the assignment makes a new local and leaves the outer \$$var unchanged — pass the value as a parameter and return the new value, or use another name"
    else
      want="$(who $ctx) reads \$$var, which it assigned only inside a block that has ended (line "
      tail="here \$$var is the outer variable, not that local — declare \$$var before the block, or pass the value as a parameter and return the new value"
    fi
    if [ $rc = 0 ] || ! grep -qF -- "$want" <<< "$out" || ! grep -qF -- "$tail" <<< "$out"; then
      echo "  FAIL [$ctx] $name: rc=$rc"; echo "       got:  $(head -1 <<< "$out")"; echo "       want: …$want…"; fail=1
    fi
    [ $ctx = use ] && ! grep -qF 'use "lib_' <<< "$out" && { echo "  FAIL [$ctx] $name: the message does not name the included file"; fail=1; }
    chk=$("$LK" --check "$f" 2>&1)
    grep -qF -- "[compile]" <<< "$chk" || { echo "  FAIL [$ctx] $name: lk --check did not report it: $chk"; fail=1; }
  done
done

BASE=$(printf '%s\n%s\n' "$HDR" "$SHOW" > base.lk; "$LK" base.lk 2>&1)
for c in "${ALLOWED[@]}"; do
  name=${c%%|*}; body=${c#*|}
  for ctx in fn cl use; do
    n=$((n+1)); f=$(make $ctx "$name" "$body")
    out=$("$LK" "$f" 2>&1); rc=$?
    [ $rc = 0 ] && [ "$out" = "$BASE" ] || { echo "  FAIL [$ctx] $name must load and keep the outer values: rc=$rc got '$out' want '$BASE'"; fail=1; }
  done
done

# A parameter with the same name as an outer variable is the function's own.
cat > param.lk <<'E'
$n = 1
function p($n) { $n = $n + 1
  $n += 10
  return $n }
print(p(5) . " " . $n)
E
n=$((n+1)); out=$("$LK" param.lk 2>&1)
[ "$out" = "16 1" ] || { echo "  FAIL parameter named like an outer variable: got '$out' want '16 1'"; fail=1; }

# The top level itself is not a function: a block there updates the outer variable.
cat > top.lk <<'E'
$n = 1
if (true) { $n = $n + 1 }
$k = 0
while ($k < 2) { $n += 1
  $k = $k + 1 }
print($n)
E
n=$((n+1)); out=$("$LK" top.lk 2>&1)
[ "$out" = "4" ] || { echo "  FAIL top-level blocks: got '$out' want '4'"; fail=1; }

# The web server does not start such a program.
if [ -n "$FCGI" ]; then
  n=$((n+1))
  printf '$hits = 0\nroute("GET", "/", function() { $hits = $hits + 1; return response::text("" . $hits) })\n' > web.lk
  timeout 10 "$FCGI" --mode http --port 7932 --workers 1 web.lk > web.log 2>&1; rc=$?
  { [ $rc != 0 ] && [ $rc != 124 ] && grep -qF 'closure reads $hits from outside it and also assigns $hits' web.log; } \
    || { echo "  FAIL web server: rc=$rc (want a start failure naming \$hits)"; tail -3 web.log; fail=1; }
fi

[ $fail = 0 ] && echo "PASS: a function cannot both read an outer variable and assign the same name ($n cases)" \
              || echo "FAIL: a function read an outer variable and assigned the same name silently"
exit $fail
