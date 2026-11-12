#!/usr/bin/env bash
# `lk --check` only loads and compiles: it never runs the program. The Plesk extension runs it on
# every site before moving the site to a new LOOK release, so a site's setup code (database
# migrations, files it writes, jobs it queues) must not run during the check — a site that stays on
# the old release would otherwise have its data changed by the new one.
# The program below writes a file, prints, appends to a log and creates a SQLite database at its top
# level, in a function it calls, and in a file it uses; after --check none of that may exist.
# Positive control: running it (without --check) does all of it.
# Usage: check_no_side_effects_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0

cat > "$T/lib.lk" <<'EOF'
file::put("from_use.txt", "used file ran")
function helper() {
  file::put("from_fn.txt", "function ran")
  return 1
}
EOF
cat > "$T/app.lk" <<'EOF'
use "lib.lk"
use file
print("TOP-LEVEL PRINT")
file::put("top.txt", "top level ran")
file::append("log.txt", "appended\n")
$n = helper()
$db = db::connect("sqlite://./side.db")
db::exec($db, "CREATE TABLE t (x INTEGER)")
route("GET", "/", function() { file::put("route.txt", "route ran") })
EOF

# logs/ is LOOK's own log directory, not the program's doing
files() { (cd "$T" && ls -1 | grep -vE '^(app|lib)\.lk$|^logs$' | sort | tr '\n' ' '); }

out=$(cd "$T" && LOOK_FILE_ROOT="$T" timeout 20 "$LK" --check app.lk 2>&1); rc=$?
left=$(files)
if [ $rc = 0 ] && ! echo "$out" | grep -q 'TOP-LEVEL PRINT' && [ -z "$left" ]; then
  echo "  OK   --check ran nothing (rc=0, no output from the program, no files)"
else
  echo "  FAIL --check: rc=$rc printed=[$(echo "$out" | grep -c 'TOP-LEVEL PRINT')] files=[$left]"; fail=1
fi

# Positive control: the same program, run, has every side effect the check must not have.
out=$(cd "$T" && LOOK_FILE_ROOT="$T" timeout 20 "$LK" app.lk 2>&1); rc=$?
left=$(files)
want="from_fn.txt from_use.txt log.txt side.db top.txt "
if echo "$out" | grep -q 'TOP-LEVEL PRINT' && [ "$left" = "$want" ]; then
  echo "  OK   positive control: running it prints and writes [$left]"
else
  echo "  FAIL positive control: rc=$rc files=[$left] want [$want]"; echo "$out" | tail -3; fail=1
fi

[ $fail = 0 ] && echo "PASS: lk --check never runs the program" || { echo "FAIL: --check side effects"; exit 1; }
