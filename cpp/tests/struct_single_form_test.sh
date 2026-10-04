#!/usr/bin/env bash
# LOOK 2: a struct field has ONE spelling — "name type [= default]". The two v1 spellings
# (an untyped field, and "name: default") are parse errors that say what to write instead.
# Usage: struct_single_form_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
check() {  # label source want-substring
    printf '%s\n' "$2" > "$TMP/t.lk"
    for env in "LOOK_VM_STRICT=1" "LOOK_CLI_VM=0"; do
        out="$(env $env "$LK" "$TMP/t.lk" 2>&1)"
        case "$out" in *"$3"*) echo "  OK   [$env] $1";; *) echo "  FAIL [$env] $1: $(echo "$out" | tr '\n' ' ' | cut -c1-200)"; fail=1;; esac
    done
}
check "untyped field is refused"      'struct A { name }'            "field 'name' needs a type. Write 'name <type>'"
check "untyped field, then another"   'struct A { a, b int }'        "field 'a' needs a type"
check "v1 'name: default' is refused" 'struct A { role: "member" }'  "field 'role' uses the old 'name: default' form. Write 'role <type> = <default>'"
check "typed field parses"            'struct A { n int = 1 }
$a = A{}
print("n=" . $a.n)'                                                   "n=1"
check "any takes every value"         'struct A { v any }
$a = A{v: [1]}
$a.v = "s"
print("v=" . $a.v)'                                                   "v=s"
[ $fail = 0 ] && echo "PASS: struct fields have a single form" || { echo "FAIL: struct single form"; exit 1; }
