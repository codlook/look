#!/usr/bin/env bash
# Davranışsal .lk testleri — gömülü SQLite / in-process (harici sunucu YOK, CI-koşabilir).
# Kapsam: db:: CRUD+tx · jobs:: lifecycle+retry · parallel() fan-out+izolasyon · prepared
# bind matrisi. Her .lk fail-loud ("FAIL " basar) + empty-green ("ran=N" + EXPECTED sabiti).
#
# POZİTİF KONTROL (Kural 1): bir .lk'de beklenen değer bozulursa "FAIL ..." basılır ve bu
# runner exit 1 döner (ölçüldü). "ran=N" satırı yoksa (script erken öldü) da FAIL.
set -u
LK="${1:-./build/lk}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

run_lk() {
  local name="$1"; local file="$2"; shift 2
  local out; out=$(env "$@" timeout 60 "$LK" "$HERE/behavioral/$file" 2>&1); local rc=$?
  if [ "$rc" -ge 128 ]; then echo "  FAIL $name: cokme (exit=$rc)"; fail=1; return; fi
  if echo "$out" | grep -q '^FAIL '; then echo "  FAIL $name: assert kirildi"; echo "$out" | grep '^FAIL ' | sed 's/^/    /'; fail=1; return; fi
  if echo "$out" | grep -q 'FAIL harness'; then echo "  FAIL $name: empty-green (assert sayisi tutmadi)"; fail=1; return; fi
  if ! echo "$out" | grep -q 'ran='; then echo "  FAIL $name: 'ran=' yok — script erken oldu"; echo "$out" | tail -2 | sed 's/^/    /'; fail=1; return; fi
  echo "  OK $name ($(echo "$out" | grep -oE 'ran=[0-9]+' | head -1))"
}

run_lk "db:: CRUD+tx"          db_crud.lk
run_lk "prepared bind matrisi" db_bind_sqlite.lk
run_lk "jobs:: lifecycle"      jobs_lifecycle.lk  "JOBS_DB=$TMP/jobs.db"
run_lk "parallel() fan-out"    parallel_fanout.lk
run_lk "string:: ops"          string_ops.lk
run_lk "array:: ops"           array_ops.lk
run_lk "type:: ops"            type_ops.lk
run_lk "queue:: FIFO"          queue_ops.lk
run_lk "file:: FS+traversal"   file_ops.lk  "LOOK_FILE_ROOT=$TMP" "BT_DIR=$TMP"
run_lk "template:: XSS-kaçış"  template_escape.lk
run_lk "validator:: sınır"     validator_check.lk

[ $fail = 0 ] && echo "PASS: davranissal (db/jobs/parallel/prepared, SQLite CI)" || echo "FAIL: davranissal"
exit $fail
