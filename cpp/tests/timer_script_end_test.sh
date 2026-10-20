#!/usr/bin/env bash
# LOOK 2: a command-line script ends when its last line has run (Go/Rust: the program's life is
# its top level; waiting is written out), and the timers it leaves behind are named, not lost
# without a word.
#   * a timer::after that has not fired when the script ends does not run; one WARN line says so
#     and says to call timer::wait()
#   * a timer::every still set when the script ends: one WARN line, says timer::cancel($id)
#   * timer::wait() waits until every timer::after callback has run and returns true;
#     timer::wait(ms) returns false when ms runs out; it does not wait for timer::every
#   * a script that waited, or cancelled its timer, writes no warning
#   * exit() ends the script too: the same warning
#   * in a web application timer::wait() is an error (a request does not wait for timers)
#   * a parallel() task still running when the script ends stops with it, named in a WARN line;
#     parallel::wait() lets it finish (it used to be waited for up to 5 s without a word)
# Usage: timer_script_end_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7756}"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
run() { out="$(cd "$TMP" && "$LK" "$1" 2>"$TMP/err.txt")"; rc=$?; out="$(printf '%s' "$out" | tr -d '\r')"; err="$(tr -d '\r' < "$TMP/err.txt")"; }   # Windows: CRLF
nwarn() { echo "$err" | grep -c 'WARN'; }

printf 'timer::after(300, function() { print("after fired") })\nprint("end")\n' > "$TMP/a.lk"
run a.lk
if [ "$out" = end ] && [ "$(nwarn)" = 1 ] && echo "$err" | grep -qF '1 timer::after callback never ran: the script ended first. To wait for them, call timer::wait()'; then
    ok "a timer::after left when the script ends: it does not run, one warning says to call timer::wait()"
else bad "a.lk: out=[$out] err=[$err]"; fi

printf '$id = timer::every(50, function() { print("tick") })\nprint("end")\n' > "$TMP/e.lk"
run e.lk
if [ "$(nwarn)" = 1 ] && echo "$err" | grep -qF '1 timer::every timer was still set when the script ended' && echo "$err" | grep -qF 'timer::cancel($id)'; then
    ok "a timer::every still set: one warning, names timer::cancel(\$id)"
else bad "e.lk: out=[$out] err=[$err]"; fi

printf 'timer::after(100, function() { print("first") })\ntimer::after(200, function() { print("second") })\n$id = timer::every(30, function() { })\n$ok = timer::wait()\ntimer::cancel($id)\nprint("waited={$ok}")\n' > "$TMP/w.lk"
run w.lk
if [ "$out" = "$(printf 'first\nsecond\nwaited=true')" ] && [ -z "$err" ]; then
    ok "timer::wait() runs every timer::after first (not waiting for timer::every); with the every cancelled, no warning"
else bad "w.lk: out=[$out] err=[$err]"; fi

printf 'timer::after(3000, function() { print("late") })\n$ok = timer::wait(100)\nprint("waited={$ok}")\n' > "$TMP/t.lk"
s=$(date +%s%N); run t.lk; ms=$(( ($(date +%s%N) - s) / 1000000 ))
if [ "$out" = "waited=false" ] && [ "$ms" -lt 2000 ] && [ "$(nwarn)" = 1 ]; then
    ok "timer::wait(100) returns false when the time runs out (${ms} ms), the callback left is named"
else bad "t.lk: out=[$out] ms=$ms err=[$err]"; fi

printf 'timer::after(300, function() { print("x") })\nprint("end")\nexit(3)\n' > "$TMP/x.lk"
run x.lk
if [ "$rc" = 3 ] && [ "$(nwarn)" = 1 ] && echo "$err" | grep -qF 'never ran'; then
    ok "exit() ends the script: exit status kept, same warning"
else bad "x.lk: rc=$rc err=[$err]"; fi

# parallel() follows the same rule (it used to be waited for up to 5 s without a word)
printf 'parallel(function() { $i = 0; while ($i < 30000000) { $i = $i + 1 } ; print("task done") })\nprint("end")\n' > "$TMP/p1.lk"
run p1.lk
if [ "$out" = end ] && [ "$(nwarn)" = 1 ] && echo "$err" | grep -qF '1 parallel() task was still running when the script ended and stopped with it. To wait for them, call parallel::wait()'; then
    ok "a parallel() task still running when the script ends: it stops, one warning says to call parallel::wait()"
else bad "p1.lk: out=[$out] err=[$err]"; fi
printf 'parallel(function() { $i = 0; while ($i < 3000000) { $i = $i + 1 } ; print("task done") })\nparallel::wait()\nprint("end")\n' > "$TMP/p2.lk"
run p2.lk
if [ "$out" = "$(printf 'task done\nend')" ] && [ -z "$err" ]; then
    ok "parallel::wait() lets the task finish; no warning"
else bad "p2.lk: out=[$out] err=[$err]"; fi

printf 'print("plain")\n' > "$TMP/p.lk"
run p.lk
[ "$out" = plain ] && [ -z "$err" ] && ok "a script without timers writes no warning" || bad "p.lk: out=[$out] err=[$err]"

printf 'route("GET", "/", function() { timer::wait(); return response::text("x") })\n' > "$TMP/web.lk"
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 web.lk > "$TMP/web.log" 2>&1 ) & pid=$!
for i in $(seq 1 60); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.05; done
code="$(curl -s -o /dev/null -w '%{http_code}' -m 3 "localhost:$PORT/")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$code" = 500 ] && grep -qF 'timer::wait() can be used only on the command line' "$TMP/web.log"; then
    ok "web: timer::wait() in a route is an error (500, the reason in the log)"
else bad "web: code=$code log=[$(grep -i error "$TMP/web.log" | head -1)]"; fi

[ $fail = 0 ] && echo "PASS: a script ends when its last line has run; timers left are named" || { echo "FAIL: timers at script end"; exit 1; }
