#!/usr/bin/env bash
# closure_semantics_test.sh — closure capture guard (bug 58).
#
# Each case is checked against an expected VALUE (the WANT table below); the 12 cases cover all the
# corners of closure semantics. (It used to be tree-walk vs VM parity; LOOK has
# a single engine.)
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/A.lk" <<'LK'
$v=5
$f=fn()=>$v
$v=10
print($f())
LK
cat > "$TMP/B.lk" <<'LK'
function m(){ $v=5; $f=fn()=>$v; $v=10; return $f() }
print(m())
LK
cat > "$TMP/C.lk" <<'LK'
use array
$fns=[]
$i=0
while($i<3){ $v=$i; $fns = array::push($fns, fn()=>$v); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
cat > "$TMP/D.lk" <<'LK'
use array
function m(){
  $fns=[]
  $i=0
  while($i<3){ $v=$i; $fns = array::push($fns, fn()=>$v); $i=$i+1 }
  return $fns[0]() . $fns[1]() . $fns[2]()
}
print(m())
LK
cat > "$TMP/E.lk" <<'LK'
use array
$fns=[]
$i=0
while($i<3){ $fns = array::push($fns, fn()=>$i); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
cat > "$TMP/F.lk" <<'LK'
$v=5
$g=function(){ $v=99 }
$g()
print($v)
LK
cat > "$TMP/G.lk" <<'LK'
function m(){ $v=5; $g=function(){ $v=99 }; $g(); return $v }
print(m())
LK
# H = C2: the variable is defined OUTSIDE the loop → NO per-iter, all closures
# see the last value (222). The fix's 2c-narrow condition (defined in the loop body) must NOT TRIGGER →
# $v stays global → parity. This locks that the fix does not break route/setup vars
# (outside the loop) (safety condition a).
cat > "$TMP/H.lk" <<'LK'
use array
$v=99
$fns=[]
$i=0
while($i<3){ $v=$i; $fns = array::push($fns, fn()=>$v); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
# I = catch variable capture (58 symmetry). The $err catch-var goes through declare_local
# but LOAD_EXC wrote the raw value → the boxed slot was taken for a cell and the capture read null.
# Fix: allocate a cell for the boxed catch-var + put the exception in cell[0]. In a loop → per-iter →
# e0e1e2 (BEFORE the fix the VM gave null null null). Locks the non-assignment declaration paths of the cell.
cat > "$TMP/I.lk" <<'LK'
use array
$fns=[]
$i=0
while($i<3){
  try { throw ("e" . $i) } catch($err) { $fns = array::push($fns, fn()=>$err) }
  $i=$i+1
}
print($fns[0]() . $fns[1]() . $fns[2]())
LK
# J = PARAMETER capture + mutation AFTER the capture (58 symmetry). The param skips declare_local in the constructor
# → does not enter boxed_slots, no cell is allocated → the capture reads a snapshot.
# The closure is built FIRST and mutated AFTER → tw by-ref 105, BEFORE the fix vm snapshot 5.
# (A mutation BEFORE the capture MASKS the bug: snapshot = the mutated value — which is why the
# test with the mutation AFTER is required.) Fix: a boxed param is boxed into a cell in the prologue.
cat > "$TMP/J.lk" <<'LK'
function t($p){ $f=fn()=>$p; $p=$p+100; return $f() }
print(t(5))
LK
# K = FOREACH variable capture + mutation AFTER the capture (58 symmetry). The foreach var skips declare_local
# through manual registration (locals_.push_back) → no cell. Fix: the boxed loop
# var gets a separate cell slot, a fresh cell every iter after FOR_STEP. tw=101,102; BEFORE the fix vm=1,2.
cat > "$TMP/K.lk" <<'LK'
use array
$fns=[]
foreach([1,2] as $v){ $fns = array::push($fns, fn()=>$v); $v=$v+100 }
print($fns[0]() . "," . $fns[1]())
LK

# L = a NAMED nested function captures an outer variable + mutation after the capture.
# compile_func_decl emitted MAKE_CLOSURE WITHOUT the capture setup → the LOAD_CAPTURE in the proto
# CRASHED at runtime with "Capture index out of range" (VM crash, while tw ran).
# Fix: compile_closure's capture-loading block was also added to compile_func_decl. 17 parity
# (BEFORE the fix vm crash). Locks the named-fn capture path, NOT the anonymous closure.
cat > "$TMP/L.lk" <<'LK'
function outer($p){ function inner(){ return $p } $p=$p+7; return inner() }
print(outer(10))
LK

# a closure captures the VALUE (at the moment it was created) — include/look/capture_check.h.
#   at top level, a name without use = global access, stays LIVE .............. A=10  E=333  H=222
#   inside a function, a name born in a loop, a catch/param/foreach value ..... B=5 C=012 D=012
#                                                                        I=e0e1e2 J=5 K=1,2
#   a plain assignment inside a closure creates its own local ................ F=5  G=5
#   a named inner function is a closure too ................................... L=10
# Old state (58): the VM was made to match the interpreter's "capture the variable" behavior with cells;
# B and J gave 10/105, L gave 17.
declare -A WANT=([A]=10 [B]=5 [C]=012 [D]=012 [E]=333 [F]=5 [G]=5 [H]=222 [I]=e0e1e2 [J]=5 [K]=1,2 [L]=10)
for c in A B C D E F G H I J K L; do
  got=$(timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  if [ "$got" = "${WANT[$c]} " ]; then
    echo "  PASS [$c] $got"
  else
    echo "  FAIL [$c] got=[$got] want=[${WANT[$c]}]"; fail=1
  fi
done
[ $fail = 0 ] && echo "PASS: closure semantics (12 cases, expected values)" || echo "FAIL: closure semantics"
exit $fail
