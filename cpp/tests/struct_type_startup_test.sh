#!/usr/bin/env bash
# LOOK 2: a misspelled field type must stop the web application at START-UP, not when the
# one route that builds the struct is finally called (possibly days later, in production).
# Structs may refer to structs declared later, so the check runs once setup has finished
# and every struct is known.
# Usage: struct_type_startup_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7720}"
fail=0
run() {  # label app-source expect(start|refuse) want-substring
    printf '%s\n' "$2" > "$TMP/app.lk"
    for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
        ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        up=""; for i in $(seq 1 25); do [ "$(curl -s -m 1 "localhost:$PORT/ok")" = "ok" ] && { up=yes; break; }; kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        if [ "$3" = start ]; then
            [ "$up" = yes ] && echo "  OK   [$mode] $1: started" || { echo "  FAIL [$mode] $1: did not start: $(tail -2 "$TMP/log.txt" | tr '\n' ' ' | cut -c1-180)"; fail=1; }
        else
            if [ -z "$up" ] && grep -q "$4" "$TMP/log.txt"; then echo "  OK   [$mode] $1: refused to start"
            else echo "  FAIL [$mode] $1: up=[$up] log: $(tail -2 "$TMP/log.txt" | tr '\n' ' ' | cut -c1-180)"; fail=1; fi
        fi
    done
}
run "misspelled type in a struct no route builds" 'struct Rare { name strng }
route("GET", "/ok", fn() => response::text("ok"))' refuse "struct 'Rare': field 'name' has unknown type 'strng'"
run "struct declared later is a known type" 'struct Later { next Node }
struct Node { v int }
route("GET", "/ok", fn() => response::text("ok"))' start ""
[ $fail = 0 ] && echo "PASS: struct field types are checked at start-up" || { echo "FAIL: struct type start-up check"; exit 1; }
