#!/usr/bin/env bash
# A Linux binary carries its memory-safety protections: it is a position-independent executable (ASLR
# places it at a random address), its relocation table is read-only once it starts (full RELRO,
# `-z relro -z now`), its stack is not executable, and stack-protector checks and fortified libc
# calls are present.
#
# The flags are set in CMakeLists.txt (LOOK_HARDEN). They have to stay: a toolchain change, a
# new static library or a changed link line can drop one of them without any test noticing — the
# 1.1.5 binary shipped without PIE, with partial RELRO and no fortified calls until this was
# measured. The check reads the finished binary, not the build settings.
# Usage: binary_hardening_check.sh <binary>...   (needs readelf and objdump)
fail=0
[ "$#" -ge 1 ] || { echo "usage: $0 <binary>..."; exit 2; }
for f in "$@"; do
    [ -f "$f" ] || { echo "  FAIL $f: no such file"; fail=1; continue; }
    name=$(basename "$f")
    type=$(readelf -h "$f" 2>/dev/null | sed -n 's/^ *Type: *//p')
    case "$type" in DYN*) echo "  OK   $name: position-independent executable" ;;
        *) echo "  FAIL $name: not a PIE (ELF type: $type) — the binary sits at a fixed address"; fail=1 ;; esac
    if readelf -l "$f" 2>/dev/null | grep -q GNU_RELRO && readelf -d "$f" 2>/dev/null | grep -qE 'BIND_NOW|FLAGS.*NOW'; then
        echo "  OK   $name: full RELRO (relocations read-only, bound at start)"
    else echo "  FAIL $name: RELRO is missing or partial (-Wl,-z,relro,-z,now)"; fail=1; fi
    if readelf -l "$f" 2>/dev/null | grep GNU_STACK | grep -q RWE; then
        echo "  FAIL $name: the stack is executable"; fail=1
    else echo "  OK   $name: non-executable stack"; fi
    asm=$(objdump -d "$f" 2>/dev/null)
    n=$(printf '%s\n' "$asm" | grep -c 'call.*__stack_chk_fail')
    if [ "$n" -ge 100 ]; then echo "  OK   $name: $n stack-protector checks"
    else echo "  FAIL $name: only $n stack-protector checks (-fstack-protector-strong)"; fail=1; fi
    n=$(printf '%s\n' "$asm" | grep -E 'call.*__[a-z0-9]+_chk' | grep -vc stack_chk)
    if [ "$n" -ge 1 ]; then echo "  OK   $name: $n fortified libc calls"
    else echo "  FAIL $name: no fortified libc calls (_FORTIFY_SOURCE)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: binary hardening" || { echo "FAIL: binary hardening"; exit 1; }
