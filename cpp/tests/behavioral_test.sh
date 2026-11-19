#!/usr/bin/env bash
# Behavioral .lk tests — embedded SQLite / in-process (NO external server, runnable in CI).
# Scope: db:: CRUD+tx · jobs:: lifecycle+retry · parallel() fan-out+isolation · prepared
# bind matrix. Every .lk is fail-loud (prints "FAIL ") + empty-green ("ran=N" + an EXPECTED constant).
#
# POSITIVE CONTROL (Rule 1): if an expected value in a .lk is broken, "FAIL ..." is printed and this
# runner returns exit 1 (measured). It is also a FAIL if the "ran=N" line is missing (the script died early).
set -u
LK="${1:-./build/lk}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

run_lk() {
  local name="$1"; local file="$2"; shift 2
  local out; out=$(env "$@" timeout 60 "$LK" "$HERE/behavioral/$file" 2>&1); local rc=$?
  if [ "$rc" -ge 128 ]; then echo "  FAIL $name: crashed (exit=$rc)"; fail=1; return; fi
  if echo "$out" | grep -q '^FAIL '; then echo "  FAIL $name: assertion failed"; echo "$out" | grep '^FAIL ' | sed 's/^/    /'; fail=1; return; fi
  if echo "$out" | grep -q 'FAIL harness'; then echo "  FAIL $name: empty-green (assertion count did not match)"; fail=1; return; fi
  if ! echo "$out" | grep -q 'ran='; then echo "  FAIL $name: 'ran=' missing — script exited early"; echo "$out" | tail -2 | sed 's/^/    /'; fail=1; return; fi
  echo "  OK $name ($(echo "$out" | grep -oE 'ran=[0-9]+' | head -1))"
}

run_lk "db:: CRUD+tx"          db_crud.lk
run_lk "prepared bind matrisi" db_bind_sqlite.lk
run_lk "jobs:: lifecycle"      jobs_lifecycle.lk  "JOBS_DB=$TMP/jobs.db" "LOOK_JOBS_RETRY_SECONDS=0"
run_lk "parallel() fan-out"    parallel_fanout.lk
run_lk "string:: ops"          string_ops.lk
run_lk "array:: ops"           array_ops.lk
run_lk "type:: ops"            type_ops.lk
run_lk "queue:: FIFO"          queue_ops.lk
run_lk "file:: FS+traversal"   file_ops.lk  "LOOK_FILE_ROOT=$TMP" "BT_DIR=$TMP"
run_lk "template:: XSS escaping"  template_escape.lk
run_lk "template:: cache"   template_cache.lk  "TEMPLATE_DIR=$TMP" "LOOK_FILE_ROOT=$TMP" "BT_DIR=$TMP"
run_lk "validator:: limits"     validator_check.lk

[ $fail = 0 ] && echo "PASS: behavioral (db/jobs/parallel/prepared, SQLite CI)" || echo "FAIL: behavioral"
exit $fail
