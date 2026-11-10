#!/bin/bash
# A function — named, a closure, or one that comes from a `use "file.lk"` — cannot change a
# variable outside it in place. Every way the VM writes into a variable in place is listed here
# (SET_PATH / PUSH_PATH / POP_PATH with a top-level root, and ++/-- on a top-level name); each must
# stop the program at load with the same message, in all three contexts. Plain `$x = …`, `$x += …`
# and `$x .= …` inside a function make a local (unchanged); reading an outer variable still works.
# Usage: outer_write_test.sh <lk> [<lk-fcgi>]
LK=${1:?usage: outer_write_test.sh <lk> [<lk-fcgi>]}; FCGI=$2
LK=$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")
[ -n "$FCGI" ] && FCGI=$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; cd "$T"
fail=0; n=0

HDR='struct S { x any }
$t = ["k" => 1, "n" => ["x" => 1], "c" => 1, "s" => "a", "arr" => [1, 2]]
$s = S { x: 1 }
$l = [1, 2]
$n = 1
$w = "a"'
SHOW='print(json::encode($t) . " " . $s.x . " " . json::encode($l) . " " . $n . " " . $w)'
printf '%s\n%s\n' "$HDR" "$SHOW" > base.lk
BASE=$("$LK" base.lk 2>&1)

# name | code | expected reason in the message
CHANGES=(
  'elem|$t["k"] = 9|an element or field of it is assigned'
  'nested|$t["n"]["x"] = 9|an element or field of it is assigned'
  'field|$s.x = 9|an element or field of it is assigned'
  'plus_eq_elem|$t["c"] += 1|an element or field of it is changed'
  'concat_eq_elem|$t["s"] .= "z"|an element or field of it is changed'
  'postinc_elem|$t["c"]++|an element or field of it is changed'
  'preinc_elem|++$t["c"]|an element or field of it is changed'
  'push|push($l, 9)|push() changes it'
  'push_path|push($t["arr"], 9)|push() changes it'
  'pop|pop($l)|pop() changes it'
  'pop_path|pop($t["arr"])|pop() changes it'
  'postinc_var|$n++|it is changed with ++'
  'preinc_var|++$n|it is changed with ++'
  'postdec_var|$n--|it is changed with --'
)
LOCALS=('assign_var|$n = 100' 'plus_eq_var|$n += 1' 'concat_eq_var|$w .= "z"')

make() {  # make <ctx> <name> <code> → file name
  case $1 in
    fn)  printf '%s\nfunction f() { %s }\nf()\n%s\n' "$HDR" "$3" "$SHOW" > "fn_$2.lk"; echo "fn_$2.lk" ;;
    cl)  printf '%s\n$c = function() { %s }\n$c()\n%s\n' "$HDR" "$3" "$SHOW" > "cl_$2.lk"; echo "cl_$2.lk" ;;
    use) printf 'function g() { %s }\n' "$3" > "lib_$2.lk"
         printf '%s\nuse "lib_%s.lk"\ng()\n%s\n' "$HDR" "$2" "$SHOW" > "use_$2.lk"; echo "use_$2.lk" ;;
  esac
}
who() { case $1 in fn) echo 'line 7: function f()';; cl) echo 'line 7: closure';; use) echo 'use "lib_'"$2"'.lk": line 1: function g()';; esac; }

for c in "${CHANGES[@]}"; do
  IFS='|' read -r name code how <<< "$c"
  var=$(echo "$code" | grep -oE '\$[a-z]+' | head -1)
  for ctx in fn cl use; do
    n=$((n+1)); f=$(make $ctx "$name" "$code")
    out=$("$LK" "$f" 2>&1); rc=$?
    want="$(who $ctx "$name") changes $var, a variable outside it ($how); a function cannot change variables outside it — pass the value as a parameter and return the new value"
    if [ $rc = 0 ] || ! grep -qF -- "$want" <<< "$out"; then
      echo "  FAIL [$ctx] $code: rc=$rc"; echo "       got:  $(head -1 <<< "$out")"; echo "       want: Error: $want"; fail=1
    fi
    chk=$("$LK" --check "$f" 2>&1)
    grep -qF -- "[compile]" <<< "$chk" || { echo "  FAIL [$ctx] $code: lk --check did not report it: $chk"; fail=1; }
  done
done

for c in "${LOCALS[@]}"; do
  IFS='|' read -r name code <<< "$c"
  for ctx in fn cl use; do
    n=$((n+1)); f=$(make $ctx "$name" "$code")
    out=$("$LK" "$f" 2>&1); rc=$?
    [ $rc = 0 ] && [ "$out" = "$BASE" ] || { echo "  FAIL [$ctx] $code makes a local: rc=$rc got '$out' want '$BASE'"; fail=1; }
  done
done

# Reading outer variables, writing own locals and parameters, and top-level blocks still work.
cat > ok.lk <<'E'
$t = ["k" => 1]
$l = [1, 2]
function reads() { return $t["k"] + count($l) }
function own($p) { $a = [1]; $a[0] = 5; push($a, 6); $a[0]++; $p[0] = 9; push($p, 1); return [$a, $p] }
$c = fn($x) => $x + $t["k"]
if (true) { $b = [1]; $b[0] = 2; push($b, 3); print(json::encode($b)) }
$t["k"] = 2
push($l, 3)
print(reads() . " " . json::encode(own([0])) . " " . $c(1) . " " . json::encode($t) . " " . json::encode($l))
E
n=$((n+1)); out=$("$LK" ok.lk 2>&1)
[ "$out" = $'[2,3]\n5 [[6,6],[9,1]] 3 {"k":2} [1,2,3]' ] || { echo "  FAIL reading / own locals / top level: got '$out'"; fail=1; }

# The web server does not start such a program, and says why.
if [ -n "$FCGI" ]; then
  n=$((n+1))
  printf '$memo = ["base" => ""]\nroute("GET", "/", function() { $memo["base"] = "x"; return response::text("ok") })\n' > web.lk
  timeout 10 "$FCGI" --mode http --port 7931 --workers 1 web.lk > web.log 2>&1; rc=$?
  { [ $rc != 0 ] && [ $rc != 124 ] && grep -qF 'closure changes $memo, a variable outside it' web.log; } \
    || { echo "  FAIL web server: rc=$rc (want a start failure naming \$memo)"; tail -3 web.log; fail=1; }
fi

[ $fail = 0 ] && echo "PASS: a function cannot change a variable outside it ($n cases: ${#CHANGES[@]} ways x 3 contexts, locals, reads, web)" \
              || echo "FAIL: a function changed or was allowed to change a variable outside it"
exit $fail
