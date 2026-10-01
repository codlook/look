#!/usr/bin/env bash
# The compiler may not narrow a value into a bytecode operand with a bare cast or mask.
# Four silent wrap-arounds came from exactly that (field name, closure, argument count,
# parameter index). Every narrowing goes through u8()/u16()/hi8()/lo8()/hint_u8()/
# i8_bits() in compiler.h, which raise a compile error when the value does not fit.
# Usage: no_unchecked_narrowing.sh [compiler.cpp]
F="${1:-src/compiler.cpp}"
# Strip // comments, then look for bare narrowing.
bad="$(sed 's://.*$::' "$F" | grep -nE '\((u?int8_t|u?int16_t)\)|static_cast<(u?int8_t|u?int16_t)>|& *0x[Ff]{2}\b' || true)"
if [ -n "$bad" ]; then
    echo "FAIL: unchecked narrowing in $F (use u8/u16/hi8/lo8 from compiler.h):"
    echo "$bad"
    exit 1
fi
echo "PASS: no unchecked narrowing in $F"
