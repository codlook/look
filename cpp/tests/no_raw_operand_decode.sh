#!/usr/bin/env bash
# Bytecode operands are combined only through the helpers in bytecode.h.
#
# Two operands form a 32-bit field (constant index, jump target, built-in index, closure
# index). When the operand width changed from 8 to 16 bits (LOOK 2), one reader outside the
# VM and the compiler — the route scanner in http_main.cpp — still combined them by hand
# with the old shift and would have read the wrong constant without any error. Every file
# that reads bytecode must use Instruction::bx() / bx_lohi() / builtin_index(); a hand-made
# shift on an instruction operand (ins.b << …, … << 16 | ins.c, code[i].b << …) fails here.
# The compiler side is covered by tests/no_unchecked_narrowing.sh.
# Usage: no_raw_operand_decode.sh   (run from anywhere)
cd "$(dirname "$0")/.." || exit 1
files=$(grep -lE '#include "look/(bytecode|compiler|vm)\.h"' src/*.cpp)
INS='(\b[A-Za-z_][A-Za-z_0-9]*|\])\.(a|b|c)\b'
bad=$(grep -nE "$INS *\)? *<<|<<[^;]*\| *\(?(size_t|uint[0-9]+_t)?\)? *[A-Za-z_][A-Za-z_0-9>\[\]-]*\.(b|c)\b" $files \
      | grep -vE '^\s*[^:]+:[0-9]+:\s*//' )
if [ -n "$bad" ]; then
    echo "FAIL: an instruction operand is combined by hand (use Instruction::bx/bx_lohi/builtin_index):"
    echo "$bad" | sed 's/^/    /'
    exit 1
fi
echo "PASS: bytecode operands are combined only through the helpers ($(echo $files | wc -w) files)"
