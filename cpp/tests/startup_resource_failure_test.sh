#!/usr/bin/env bash
# When lk-fcgi cannot get its threads or stacks at startup it says why and exits non-zero.
#
# Under a virtual memory limit (shared hosting) on a many-core machine the worker and database
# threads did not all fit. lk-fcgi --mode http then either hung — alive, port never opened,
# nothing in the log (a pool constructor gave up half way and blocked destroying a condition
# variable its threads waited on) — or died in std::terminate ("terminate called without an
# active exception"). Even a clean setup error exited with status 0, which systemd's
# Restart=on-failure takes as success. Now every start either serves or exits non-zero with the
# reason, like a Go program that cannot create an OS thread.
# Usage: startup_resource_failure_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7771}"
printf 'route("GET", "/ok", fn() => response::text("alive"))\n' > "$TMP/a.lk"
fail=0; served=0; failed_loud=0; total=0
# A start that goes wrong records the state at that moment (a red that shows once must leave its
# evidence): process state and memory, threads, the port's sockets, every running lk process, the
# stacks of all threads (gdb, when present) and the end of the log.
evidence() {
  echo "    --- evidence ($1, pid $pid)"
  grep -E '^(State|VmPeak|VmSize|VmRSS|Threads)' /proc/$pid/status 2>/dev/null | sed 's/^/    /'
  command -v ss >/dev/null && ss -tanp 2>/dev/null | grep ":$PORT " | sed 's/^/    ss: /'
  ps -eo pid,ppid,stat,etime,args 2>/dev/null | grep -E '[l]k-fcgi|[l]k-cgi|/[l]k ' | sed 's/^/    ps: /'
  command -v gdb >/dev/null && kill -0 "$pid" 2>/dev/null && timeout 30 gdb -p "$pid" -batch -ex 'thread apply all bt 6' 2>/dev/null \
    | grep -E '^(Thread|#)' | head -120 | sed 's/^/    gdb: /'
  tail -5 "$TMP/log" | sed 's/^/    log: /'
}
for mode in http fcgi; do
  for mb in 1024 768 512 256; do
    for r in 1 2 3; do
      total=$((total + 1))
      if [ $mode = http ]; then
        ( cd "$TMP" && ulimit -v $((mb * 1024)) && exec "$FCGI" --mode http --port "$PORT" --workers 64 a.lk > "$TMP/log" 2>&1 ) & pid=$!
      else
        ( cd "$TMP" && ulimit -v $((mb * 1024)) && exec "$FCGI" --port "$PORT" --workers 64 a.lk > "$TMP/log" 2>&1 ) & pid=$!
      fi
      state=""
      for i in $(seq 1 80); do
        if ! kill -0 "$pid" 2>/dev/null; then state=exited; break; fi
        if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then state=listening; break; fi
        sleep 0.1
      done
      if [ "$state" = exited ]; then
        wait "$pid"; rc=$?
        if [ "$rc" != 0 ] && [ "$rc" -lt 128 ] && grep -q "could not start" "$TMP/log" && ! grep -q "terminate called" "$TMP/log"; then
          failed_loud=$((failed_loud + 1))
        else
          echo "  FAIL $mode ${mb} MB: exited $rc: $(grep -vE 'Rate limit|INFO' "$TMP/log" | tail -2 | tr '\n' ' ' | head -c 220)"; fail=1
          evidence exited
        fi
      elif [ "$state" = listening ]; then
        # The port opens only when the server can serve: a listening server must answer.
        if [ $mode = http ] && [ "$(curl -s -m 3 "localhost:$PORT/ok")" != alive ]; then
          echo "  FAIL $mode ${mb} MB: listening but no answer"; fail=1
          evidence listening
        else served=$((served + 1)); fi
      else
        echo "  FAIL $mode ${mb} MB: alive after 8 s but not listening (silent hang)"; fail=1
        evidence hang
      fi
      kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    done
  done
done
echo "  $total starts: $served served, $failed_loud exited with the reason, $((total - served - failed_loud)) wrong"
[ $fail = 0 ] && echo "PASS: every start serves or says why it cannot" || { echo "FAIL: a start hung, crashed or failed silently"; exit 1; }
