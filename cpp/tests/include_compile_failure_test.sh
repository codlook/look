#!/usr/bin/env bash
# A file loaded with `use "file.lk"` that the VM cannot compile is not dropped.
#
# The compiler swallowed every error raised while it compiled an included file and went on:
# "compile OK", with that file's functions missing. Up to 1.0.7 the failing request was then
# run again on the tree-walk engine, which hid it. Since 1.0.8 nothing is run twice, so every
# route that called a function of that file answered 500 "Undefined variable: <function>" —
# also functions that had nothing to do with the line the VM could not compile.
# Now the compile fails as a whole. LOOK 2 runs only on the VM, so the application does not
# start (exit 1), and the error names the included file and the reason — nothing is dropped
# silently and no route answers "Undefined variable".
# The included file here is valid LOOK: one call in it passes 65535 arguments, more than the VM
# encodes (tests/vm_operand_limits_test.sh).
# Usage: include_compile_failure_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7737}"
args="$(seq -s, 1 65535)"
cat > "$TMP/lib.lk" <<LK
function twice(\$n) { return \$n * 2 }
function cnt(...\$a) { return count(\$a) }
function many() { return cnt($args) }
LK
cat > "$TMP/app.lk" <<'LK'
use "lib.lk"
route("GET", "/x", fn() => response::text(twice(21)))
route("GET", "/m", fn() => response::text(many()))
LK
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    echo "  FAIL the server started with an included file the VM cannot compile"; echo "FAIL: included file not refused"; exit 1
fi
wait "$pid"; rc=$?; pid=""
named="$(grep -c 'the VM cannot run this program (compile error: use "lib.lk": .*65534' "$TMP/log.txt")"
undef="$(grep -c 'Undefined variable' "$TMP/log.txt")"
if [ "$rc" = 1 ] && [ "$named" -ge 1 ] && [ "$undef" = "0" ]; then
    echo "  OK   the application does not start, and the error names the included file and the reason"
    echo "PASS: an included file the VM cannot compile is not dropped"
else echo "  FAIL exit=$rc named=$named undefined-variable-errors=$undef log=[$(grep -iE 'error' "$TMP/log.txt" | head -2 | tr '\n' ' ')]"; echo "FAIL: included file dropped"; exit 1; fi
