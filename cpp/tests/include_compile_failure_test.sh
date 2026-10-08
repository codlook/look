#!/usr/bin/env bash
# A file loaded with `use "file.lk"` that the VM cannot compile is not dropped.
#
# The compiler swallowed every error raised while it compiled an included file and went on:
# "compile OK", with that file's functions missing. Up to 1.0.7 the failing request was then
# run again on the tree-walk engine, which hid it. Since 1.0.8 nothing is run twice, so every
# route that called a function of that file answered 500 "Undefined variable: <function>" —
# also functions that had nothing to do with the line the VM could not compile.
# Now the compile fails as a whole: the server says so at startup and runs the application
# on the tree-walk engine, where it works.
# The included file here is valid LOOK: one call in it passes 300 arguments, more than the VM
# encodes (tests/vm_operand_limits_test.sh).
# Usage: include_compile_failure_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7737}"
args="$(seq -s, 1 300)"
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
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/x" && break; sleep 0.1; done
x="$(curl -s -m 3 -w ' %{http_code}' "localhost:$PORT/x")"; m="$(curl -s -m 3 -w ' %{http_code}' "localhost:$PORT/m")"
said="$(grep -c 'The VM cannot run this program' "$TMP/log.txt")"
undef="$(grep -c 'Undefined variable' "$TMP/log.txt")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$x" = "42 200" ] && [ "$m" = "300 200" ] && [ "$said" = "1" ] && [ "$undef" = "0" ]; then
    echo "  OK   the functions of the included file work, and the server said the application runs on the tree-walk engine"
    echo "PASS: an included file the VM cannot compile is not dropped"
else echo "  FAIL /x=[$x] /m=[$m] announced=$said undefined-variable-errors=$undef"; echo "FAIL: included file dropped"; exit 1; fi
