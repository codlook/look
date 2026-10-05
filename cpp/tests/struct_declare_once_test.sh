#!/usr/bin/env bash
# LOOK 2: a struct is declared once, at the top level of a file.
#   * inside a function, a closure or a block      -> parse error
#   * same name, different fields, same file       -> parse error naming the first line
#   * same name, different fields, another file    -> error when the file is loaded
#   * the identical declaration read again         -> fine
# Why: a second, different definition of a name is a mistake (two files chose the same name,
# or a file was loaded in two versions). It was also the only way to reach the stale field
# cache fixed in e6a705e: the same line read the wrong field without an error.
# Usage: struct_declare_once_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
check() {  # label source want-substring [envs]
    printf '%s\n' "$2" > "$TMP/t.lk"
    for env in ${4:-LOOK_VM_STRICT=1 LOOK_CLI_VM=0}; do
        out="$(env $env "$LK" "$TMP/t.lk" 2>&1)"
        case "$out" in *"$3"*) echo "  OK   [$env] $1";; *) echo "  FAIL [$env] $1: $(echo "$out" | tr '\n' ' ' | cut -c1-200)"; fail=1;; esac
    done
}
TOP="must be declared at the top level of a file"
check "inside a function"  'function f() { struct A { n int } }
print("ran")'                                                        "struct 'A' $TOP"
check "inside a closure"   '$f = function() { struct A { n int } }
print("ran")'                                                        "struct 'A' $TOP"
check "inside a block"     'if (true) { struct A { n int } }
print("ran")'                                                        "struct 'A' $TOP"
check "nothing ran before the error" 'print("before")
function f() { struct A { n int } }'                                 "struct 'A' $TOP"
out="$("$LK" "$TMP/t.lk" 2>&1)"; case "$out" in *before*) echo "  FAIL output was produced before the parse error"; fail=1;; *) echo "  OK   no output before the parse error";; esac
check "different fields, same file" 'struct A { a int
  b int }
print("x")
struct A { b int
  a int }'                                                           "struct 'A' is already declared at line 1 with different fields"
check "different default, same file" 'struct A { a int = 1 }
struct A { a int = 2 }'                                              "struct 'A' is already declared at line 1 with different fields"
check "identical, same file" 'struct A { a int = 1, b string }
struct A { a int = 1
  b string }
$x = A{}
print("a=" . $x.a)'                                                  "a=1"

# Across files: the VM does not run file includes, so these run on the default CLI path
# (which hands such a script to the tree-walk engine) and on the tree-walk engine directly.
printf 'struct Shared { a int = 1\n  b int = 2 }\n' > "$TMP/same.lk"
printf 'struct Shared { b int = 2\n  a int = 1 }\n' > "$TMP/other.lk"
check "different fields, another file" 'struct Shared { a int = 1
  b int = 2 }
use "other.lk"
print("ran")'                                                        "struct 'Shared' is already declared at" "LOOK_X=1 LOOK_CLI_VM=0"
check "identical, another file" 'struct Shared { a int = 1
  b int = 2 }
use "same.lk"
$s = Shared{}
print("b=" . $s.b)'                                                  "b=2" "LOOK_X=1 LOOK_CLI_VM=0"
[ $fail = 0 ] && echo "PASS: a struct is declared once, at the top level" || { echo "FAIL: struct declare once"; exit 1; }
