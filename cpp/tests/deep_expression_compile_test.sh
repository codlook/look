#!/usr/bin/env bash
# Deep expressions and blocks compile on a small stack — or fail with the reason, never crash.
#
# The compiler walked the syntax tree recursively: a 1,000-term `1 + 1 + …` (the parser allows up
# to 1,000 operators in a chain) crashed the process with SIGSEGV on a 512 KB stack (compile_binary
# × 580 frames). Small stacks are real: the main thread on Windows is 1 MB, threads of a musl build
# are 128 KB. Binary and && / || chains are now compiled with a loop; every other nesting the
# parser allows (150 levels) is checked against the real remaining stack.
# Each shape below is the largest the parser accepts, run under several stack limits:
#   * never a crash (no signal) at any size
#   * 512 KB and up (the main thread on Windows is 1 MB): compiles and prints the right result
#   * smaller: the right result or a clean error that says to split the expression (binary chains
#     of 1,000 operators compile even at 64 KB; 149 nested blocks or assignments need ~300 KB)
# Usage: deep_expression_compile_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP"; fail=0; n=0

rep() { local i; for ((i = 0; i < $2; i++)); do printf '%s' "$1"; done; }
mk() { printf '%s\n' "$2" > "$1.lk"; }

mk chain_add      "\$x = 1$(rep ' + 1' 999)
print(\$x)"
mk chain_concat   "\$x = \"a\"$(rep ' . "a"' 999)
print(strlen(\$x))"
mk chain_and      "\$t = true
\$x = \$t$(rep ' && $t' 999)
print(\$x ? \"yes\" : \"no\")"
mk chain_mixed    "\$x = 1$(rep ' * 1 + 1' 499)
print(\$x)"
mk parens         "\$x = $(rep '(' 45)1$(rep ')' 45)
print(\$x)"
mk arrays         "\$x = $(rep '[' 45)7$(rep ']' 45)
print(count(\$x))"
mk calls          "function f(\$v) { return \$v }
print($(rep 'f(' 45)5$(rep ')' 45))"
mk ternary        "\$t = true
\$x = $(rep '$t ? 1 : ' 149)0
print(\$x)"
mk power          "\$x = 2$(rep ' ** 1' 145)
print(\$x)"
mk assign_chain   "$(rep '$a = ' 149)9
print(\$a)"
mk closures       "\$f = $(rep 'fn() => ' 45)3
print(\"ok\")"
mk blocks         "\$x = 0
$(rep 'if (true) { ' 149)\$x = 1$(rep ' }' 149)
print(\$x)"

declare -A WANT=( [chain_add]=1000 [chain_concat]=1000 [chain_and]=yes [chain_mixed]=500
                  [parens]=1 [arrays]=1 [calls]=5 [ternary]=1 [power]=2 [assign_chain]=9
                  [closures]=ok [blocks]=1 )

for f in chain_add chain_concat chain_and chain_mixed parens arrays calls ternary power assign_chain closures blocks; do
  for kb in 8192 1024 512 256 128 64; do
    n=$((n + 1))
    out=$(bash -c "ulimit -s $kb; exec '$LK' $f.lk" 2>&1); rc=$?
    if [ $rc -ge 128 ]; then
      echo "  FAIL $f @ ${kb} KB: crashed (signal $((rc - 128)))"; fail=1
    elif [ "$out" = "${WANT[$f]}" ]; then :
    elif [ $kb -ge 512 ]; then
      echo "  FAIL $f @ ${kb} KB: got '$(head -1 <<< "$out" | cut -c1-140)', want '${WANT[$f]}'"; fail=1
    elif ! grep -qE "nested too deeply to compile on this thread's stack|nested too deep|Stack overflow" <<< "$out"; then
      echo "  FAIL $f @ ${kb} KB: neither the result nor a clean error: '$(head -1 <<< "$out" | cut -c1-140)'"; fail=1
    fi
  done
done

[ $fail = 0 ] && echo "PASS: deep expressions compile on a small stack or say why, never crash ($n runs)" \
              || { echo "FAIL: a deep expression crashed or failed to compile"; exit 1; }
