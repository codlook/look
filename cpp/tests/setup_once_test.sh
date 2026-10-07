#!/usr/bin/env bash
# The top level of a web application runs once at startup, on every engine.
#
# The server runs the script's top level twice: on the tree-walk engine, and then on the VM to
# produce the route closures as bytecode. Until 1.0.8 the second pass called module functions
# again or skipped them by a list of name prefixes, so on the default engine:
#   * file::put / file::append at the top level wrote twice (two lines for one boot);
#   * crypto::uuid and the like produced a second, different value;
#   * cache::get at the top level gave null to the VM pass, so routes saw a value read at
#     setup as missing.
# Since 1.0.9 the second pass replays the results of the first, in order, and calls nothing
# again. Every mode must agree with the tree-walk engine.
# Usage: setup_once_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7723}"
cat > "$TMP/app.lk" <<'LK'
use json
use file
use cache
use crypto
use string
function boot_lines() { return count(string::split(file::read("boot.log"), "\n")) - 1 }
function note($what) { file::append("boot.log", $what . "\n"); return boot_lines() }
cache::set("boot", "set-at-setup", 600)
$from_cache = cache::get("boot") ?? "MISSING"
$token = crypto::uuid()
file::put("token.txt", $token)
$first = note("first")
$upper = array::map(["a", "b"], fn($x) => string::upper($x))
$second = note("second")
route("GET", "/n", fn() => response::json(["from_cache" => $from_cache,
    "token_is_the_one_written" => ($token == file::read("token.txt")),
    "lines_seen_at_setup" => [$first, $second], "upper" => $upper, "lines_now" => boot_lines()]))
LK
sed -i 's/^use string$/use string\nuse array/' "$TMP/app.lk"
WANT='{"from_cache":"set-at-setup","token_is_the_one_written":true,"lines_seen_at_setup":[1,2],"upper":["A","B"],"lines_now":2}'
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    rm -rf "$TMP/w"; mkdir "$TMP/w"; cp "$TMP/app.lk" "$TMP/w/"
    ( cd "$TMP/w" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/n" && break; sleep 0.1; done
    got="$(curl -s -m 3 "localhost:$PORT/n")"
    vm="$(grep -o 'VM routes: [0-9]* registered' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = "$WANT" ]; then echo "  OK   [$mode] the top level ran once; routes see the values computed at setup"
    else echo "  FAIL [$mode] $got $(grep -m1 -i 'error' "$TMP/log.txt" | cut -c1-150)"; fail=1; fi
    if [ "$mode" != "LOOK_BYTECODE=0" ]; then
        if [ "$vm" = "VM routes: 1 registered" ]; then echo "  OK   [$mode] the route is on the VM"
        else echo "  FAIL [$mode] the VM setup did not complete: '$vm' $(grep -m1 'replay\|setup' "$TMP/log.txt" | cut -c1-150)"; fail=1; fi
    fi
done
[ $fail = 0 ] && echo "PASS: the top level runs once at startup" || { echo "FAIL: top-level code ran twice or setup values differ"; exit 1; }
