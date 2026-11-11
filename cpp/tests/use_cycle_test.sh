#!/usr/bin/env bash
# `use "file.lk"` cycles are an error at load (Go: "import cycle not allowed").
#
# The main script was not on the include stack, so a.lk -> b.lk -> a.lk compiled the main file a
# second time and ran it twice ("a b a"); a cycle between included files was skipped silently.
# Now a file that uses a file still being compiled stops the program before anything runs, and
# the message shows the chain. A file used by two others (a diamond) is still compiled once.
# Usage: use_cycle_test.sh <lk> [<lk-fcgi>]
LK="$(cd "$(dirname "${1:?usage: $0 <lk> [<lk-fcgi>]}")" && pwd)/$(basename "$1")"
FCGI=""; [ -n "${2:-}" ] && FCGI="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP"; fail=0; n=0

# refused <dir> <main> <chain>: nothing runs, rc != 0, the chain is named, lk --check reports it
refused() {
  n=$((n + 1))
  local out rc chk
  out=$(cd "$1" && "$LK" "$2" 2>&1); rc=$?
  if [ $rc = 0 ] || ! grep -qF "import cycle: $3" <<< "$out" || grep -qE '^(a|b|c|self)$' <<< "$out"; then
    echo "  FAIL $1/$2: rc=$rc, want 'import cycle: $3' and nothing run:"; echo "$out" | sed 's/^/       /' | head -5; fail=1
  fi
  chk=$(cd "$1" && "$LK" --check "$2" 2>&1)
  grep -qF "import cycle: $3" <<< "$chk" || { echo "  FAIL $1/$2: lk --check did not report the cycle: $chk"; fail=1; }
}

mkdir two && cd two
printf 'print("a")\nuse "b.lk"\n' > a.lk
printf 'print("b")\nuse "a.lk"\n' > b.lk
cd ..; refused two a.lk "a.lk -> b.lk -> a.lk"

mkdir self && printf 'print("self")\nuse "self.lk"\n' > self/self.lk
refused self self.lk "self.lk -> self.lk"

mkdir three && cd three
printf 'use "b.lk"\nprint("main")\n' > main.lk
printf 'print("b")\nuse "c.lk"\n' > b.lk
printf 'print("c")\nuse "b.lk"\n' > c.lk
cd ..; refused three main.lk "b.lk -> c.lk -> b.lk"

mkdir -p sub/lib && cd sub
printf 'use "lib/x.lk"\nprint("main")\n' > main.lk
printf 'use "y.lk"\nfunction fx() { return 1 }\n' > lib/x.lk
printf 'use "../main.lk"\n' > lib/y.lk
cd ..; refused sub main.lk "main.lk -> lib/x.lk -> lib/y.lk -> main.lk"

# a diamond is not a cycle: d.lk is used by b.lk and c.lk and runs once
mkdir diamond && cd diamond
printf 'use "b.lk"\nuse "c.lk"\nprint(fb() + fc())\n' > main.lk
printf 'use "d.lk"\nfunction fb() { return fd() }\n' > b.lk
printf 'use "d.lk"\nfunction fc() { return fd() + 1 }\n' > c.lk
printf 'print("d")\nfunction fd() { return 10 }\n' > d.lk
n=$((n + 1)); out=$("$LK" main.lk 2>&1); cd ..
[ "$out" = $'d\n21' ] || { echo "  FAIL diamond: got '$out', want d once then 21"; fail=1; }

# the web server does not start a program with a cycle
if [ -n "$FCGI" ]; then
  n=$((n + 1))
  mkdir web && printf 'route("GET", "/", fn() => response::text("ok"))\nuse "lib.lk"\n' > web/app.lk
  printf 'use "app.lk"\n' > web/lib.lk
  ( cd web && timeout 10 "$FCGI" --mode http --port 7933 --workers 1 app.lk > log 2>&1 ); rc=$?
  { [ $rc != 0 ] && [ $rc != 124 ] && grep -qF "import cycle: app.lk -> lib.lk -> app.lk" web/log; } \
    || { echo "  FAIL web server: rc=$rc, want a start failure naming the cycle"; tail -3 web/log; fail=1; }
fi

[ $fail = 0 ] && echo "PASS: use cycles stop the program at load with the chain; diamonds compile once ($n cases)" \
              || { echo "FAIL: use cycle"; exit 1; }
