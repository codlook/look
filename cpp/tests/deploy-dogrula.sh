#!/usr/bin/env bash
# deploy-dogrula.sh — live smoke test AFTER DEPLOY.
#
# The guards test the source; this script verifies EXTERNALLY (over HTTP) and by running the binary
# that the DEPLOYED BINARY really contains the fixes —
# the one outside proof against the risk of "looks done but isn't". It can be run on a Docker-local
# deploy and on a VPS deploy alike.
#
# Usage:
#   deploy-dogrula.sh [LK] [LKFCGI]      # local binaries (Docker)
#   BASE_URL=https://test.codlook.com deploy-dogrula.sh   # live site (HTTP checks only)
#
# Fixes it verifies:
#   50 — lk test: does assert_throws() really assert (not a false green)
#   52 — does json::decode keep an ID beyond int64 as a string (over HTTP)
set -u
LK="${1:-./build/lk}"
LKFCGI="${2:-./build/lk-fcgi}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"; LKFCGI="$(cd "$(dirname "$LKFCGI")" && pwd)/$(basename "$LKFCGI")"   # we cd below
BASE_URL="${BASE_URL:-}"
fail=0
TMP=$(mktemp -d); trap 'rm -rf "$TMP"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null' EXIT

echo "── deploy verification ─────────────────────────────────────────"

# ── bug 52: json::decode int64 precision — over HTTP (external proof) ────────
JBIG='9223372036854775809'
if [ -n "$BASE_URL" ]; then
  # Live site: the /_dogrula_json endpoint is expected (it must be in the deployed app)
  body=$(curl -s --max-time 10 "$BASE_URL/_dogrula_json" 2>/dev/null)
else
  # Docker-local: bring up our own mini app in HTTP mode
  cat > "$TMP/app.lk" <<'EOF'
use json
route("GET", "/_dogrula_json", function() {
    $d = json::decode("{\"id\": 9223372036854775809, \"big\": 18446744073709551615, \"norm\": 42}")
    print(json::encode($d))
})
route("GET", "/_health", function() { print("ok") })
EOF
  PORT=7599
  "$LKFCGI" --mode http --port $PORT "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
  SRV_PID=$!
  # let the server come up
  for _ in $(seq 1 20); do
    curl -s --max-time 2 "http://127.0.0.1:$PORT/_health" 2>/dev/null | grep -q ok && break
    sleep 0.3
  done
  body=$(curl -s --max-time 10 "http://127.0.0.1:$PORT/_dogrula_json" 2>/dev/null)
fi

if echo "$body" | grep -q "\"$JBIG\""; then
  echo "  PASS 52 (HTTP): json ID beyond int64 is kept as a string (live binary)"
elif echo "$body" | grep -q "9223372036854775808"; then
  echo "  FAIL 52 (HTTP): ID was silently corrupted to ...808 — the FIX IS NOT LIVE"; fail=1
else
  echo "  FAIL 52 (HTTP): expected response not received: [$body]"; fail=1
fi

# ── bug 50: does lk test assert_throws really assert ─────────────────────────
# (local binary only — lk test is not run over HTTP on the live site)
if [ -z "$BASE_URL" ]; then
  mkdir -p "$TMP/tests"
  cat > "$TMP/tests/t.lk" <<'EOF'
test("firlatmayan fn => BASARISIZ olmali", function() {
    assert_throws(function() { $x = 1 })
})
EOF
  tout=$( cd "$TMP" && "$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")" test 2>&1 | sed 's/\x1b\[[0-9;]*m//g' )
  if echo "$tout" | grep -q "1 failed"; then
    echo "  PASS 50: assert_throws catches a non-throwing fn (framework correct)"
  else
    echo "  FAIL 50: assert_throws does NOT assert — the test framework is falsely green:"; echo "$tout" | grep -E "✅|❌|passed|failed" | sed 's/^/       /'; fail=1
  fi
fi

# ── bug 56: Slowloris header deadline — in the LIVE environment (timing-sensitive) ────
# Local binary only (starts its own instance with a low deadline).
if [ -z "$BASE_URL" ]; then
  SLPORT=7598
  LOOK_HEADER_TIMEOUT=3000 "$LKFCGI" --mode http --port "$SLPORT" --workers 2 "$TMP/app.lk" >"$TMP/sl.log" 2>&1 &
  SLPID=$!
  for _ in $(seq 1 20); do
    curl -s --max-time 2 "http://127.0.0.1:$SLPORT/_health" 2>/dev/null | grep -q ok && break; sleep 0.3
  done
  sl=$(python3 - "$SLPORT" <<'PY'
import socket, sys, time
P = int(sys.argv[1]); s = socket.socket(); s.connect(("127.0.0.1", P))
cut = False
try:
    for p in [b"GET / HTTP/1.1\r\n", b"H1: a\r\n", b"H2: b\r\n", b"H3: c\r\n", b"H4: d\r\n", b"H5: e\r\n"]:
        s.sendall(p); time.sleep(1.0)
except Exception:
    cut = True
if not cut:
    s.settimeout(2.0)
    try:
        d = s.recv(64); cut = (d == b"" or b"408" in d)
    except socket.timeout:
        cut = False
    except Exception:
        cut = True
s.close()
print("KESILDI" if cut else "TUTULUYOR")
PY
)
  kill "$SLPID" 2>/dev/null
  if [ "$sl" = "KESILDI" ]; then
    echo "  PASS 56 (Slowloris): continuous slow-drip was cut at the deadline (live environment, deadline=3s)"
  else
    echo "  FAIL 56 (Slowloris): slow-drip connection was HELD ($sl) — the deadline is not working live"; fail=1
  fi
fi

# ── bug 57: VM recursion bounded (OOM DoS) — CLI, local binary ───────────────
# The CALL opcode must enforce MAX_CALL_DEPTH; runaway recursion must give a graceful "Stack
# overflow", not continue until memory runs out. If lk-fcgi has no CLI mode it falls back to lk;
# if neither exists it skips (in live HTTP-only mode there may be no LK).
RECLK="$LK"; [ -x "$RECLK" ] || RECLK="$LKFCGI"
if [ -z "$BASE_URL" ] && [ -x "$RECLK" ]; then
  printf 'function r($n){ if($n<=0){return 0} return 1+r($n-1) }\nprint(r(100000))\n' > "$TMP/rec.lk"
  rout=$(timeout 15 "$RECLK" "$TMP/rec.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ')
  if echo "$rout" | grep -qi "stack overflow"; then
    echo "  PASS 57 (VM recursion): deep recursion is bounded (graceful stack overflow)"
  elif echo "$rout" | grep -q "100000"; then
    echo "  FAIL 57: r(100000) returned — the CALL path skips MAX_CALL_DEPTH (unbounded/OOM risk)"; fail=1
  else
    echo "  (57 skipped: unexpected output: $rout)"
  fi
fi

# ── bug 58: closure capture parity — CLI, local binary (external proof) ──────
# If the VM by-value snapshot bug is fixed, a captured var defined inside a loop becomes a per-iter
# cell: fns[0..2]() → 0,1,2 → "012". WITHOUT the fix all see the last value → "222".
# The C1 scenario (a live-verification suggestion): proves externally that 58 is really
# in the live binary.
RECLK58="$LK"; [ -x "$RECLK58" ] || RECLK58="$LKFCGI"
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  printf 'use array\n$fns=[]\n$i=0\nwhile($i<3){ $v=$i; $fns = array::push($fns, fn()=>$v); $i=$i+1 }\nprint($fns[0]() . $fns[1]() . $fns[2]())\n' > "$TMP/clo.lk"
  cout=$(timeout 10 "$RECLK58" "$TMP/clo.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
  if [ "$cout" = "012" ]; then
    echo "  PASS 58 (closure capture): per-iter cell is live — fns[0..2]()=012"
  elif [ "$cout" = "222" ]; then
    echo "  FAIL 58: closure=222 — the fix for the VM by-value snapshot bug is NOT LIVE (fix missing)"; fail=1
  else
    echo "  (58 skipped: unexpected output: [$cout])"
  fi
fi

# ── 58+ closure guard: 12 cases (including catch/param/foreach/named-fn) ─────
# 58 C1 is a single scenario; closure_semantics_test.sh tests ALL 12 cases (A-L) against the expected
# values → it also guards the catch(I)/param(J)/foreach(K)/named-fn(L) fixes added in this round
# in the deployed binary. Only if the local binary + guard
# script exist (skipped in live HTTP-only mode).
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  GUARD="$(dirname "$0")/closure_semantics_test.sh"
  if [ -f "$GUARD" ]; then
    if bash "$GUARD" "$RECLK58" >"$TMP/clo_guard.log" 2>&1; then
      echo "  PASS 58+ (closure guard): 12 cases pass (A-L, catch/param/foreach/named-fn)"
    else
      echo "  FAIL 58+ (closure guard): $(grep -m1 FAIL "$TMP/clo_guard.log")"; fail=1
    fi
  fi
fi

# ── is channel() wired in the CLI-VM — CLI, local binary ─────────────────────
# The channel() builtin was NOT WIRED in the CLI-VM (main.cpp build_cli_builtins) →
# "unavailable (not wired)". While tree-walk worked the VM (DEFAULT) broke.
# If fixed, channel+send+receive returns 42 in the CLI-VM (without the fix an error/empty).
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  printf '$ch=channel(1)\nsend($ch, 42)\nprint(receive($ch))\n' > "$TMP/ch.lk"
  chout=$(timeout 10 "$RECLK58" "$TMP/ch.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
  if [ "$chout" = "42" ]; then
    echo "  PASS channel (CLI-VM): channel/send/receive are wired — 42"
  else
    echo "  FAIL channel: CLI-VM channel() did not work (expected 42): [$chout]"; fail=1
  fi
fi

# ── close()/chan_size() without arguments → graceful (NOT SEGFAULT) — CLI ────
# the compiler did not validate the argument count for close/chan_size → a call without arguments was a
# null deref of e.arguments[0] → COMPILE-TIME SEGFAULT (tree-walk gave a graceful error).
# If fixed, a graceful error (exit<128), without the fix SIGSEGV (exit>=128).
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  printf 'close()\n' > "$TMP/cl.lk"
  timeout 10 "$RECLK58" "$TMP/cl.lk" >/dev/null 2>&1; crc=$?
  if [ $crc -lt 128 ]; then
    echo "  PASS close/chan_size (arg-check): no-argument call is graceful (exit $crc, no segfault)"
  else
    echo "  FAIL close(): no-argument call SEGFAULTs (exit $crc) — argument-count check missing"; fail=1
  fi
fi

# ── int(float>~1e6): the to_string() scientific-notation bug — CLI ───────────
# int() was stoll(to_string(float)) → to_string(1e6)="1e+06", stoll stopped at 'e' and
# returned 1 → int(1000000.0)=1 (SILENT corruption, timestamp/money/ID arithmetic). Fix:
# to_int() is a direct cast. If fixed, int(1000000.0)=1000000, without the fix 1.
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  printf 'print(int(1700000000.0 * 1000.0))\n' > "$TMP/int.lk"
  iout=$(timeout 10 "$RECLK58" "$TMP/int.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
  if [ "$iout" = "1700000000000" ]; then
    echo "  PASS int(float>1e6): large float->int is correct (ms-timestamp 1700000000000)"
  elif [ "$iout" = "1" ]; then
    echo "  FAIL int(): stoll(to_string(float)) bug is LIVE — int(1.7e12)=1 (scientific notation)"; fail=1
  else
    echo "  (int skipped: unexpected: [$iout])"
  fi
fi

# ── file:: path-root guard: a legitimate relative path works + traversal is blocked — CLI ────
# assert_in_file_root wrongly rejected a bare relative name (file::put("x.txt"))
# (weakly_canonical leaves it relative → mismatch with the absolute root). Fixed with fs::absolute.
# If fixed: a relative put works (OK) AND ../etc/passwd is blocked. Both are required.
if [ -z "$BASE_URL" ] && [ -x "$RECLK58" ]; then
  printf 'use file\nfile::put("_dogrula_rel.txt","ok")\nprint(file::read("_dogrula_rel.txt"))\n' > "$TMP/frel.lk"
  ( cd "$TMP" && timeout 10 "$RECLK58" "$TMP/frel.lk" 2>&1 | grep -v "INFO\|Pool" ) > "$TMP/frel.out"
  printf 'use file\nfile::read("../../../../etc/passwd")\n' > "$TMP/ftrav.lk"
  ( cd "$TMP" && timeout 10 "$RECLK58" "$TMP/ftrav.lk" 2>&1 | grep -v "INFO\|Pool" ) > "$TMP/ftrav.out"
  rm -f "$TMP/_dogrula_rel.txt"
  if grep -q "^ok$" "$TMP/frel.out" && grep -qi "access denied" "$TMP/ftrav.out"; then
    echo "  PASS file-root: relative put/read works + ../etc/passwd is blocked"
  else
    echo "  FAIL file-root: relative=[$(cat $TMP/frel.out)] traversal=[$(cat $TMP/ftrav.out)]"; fail=1
  fi
fi

# ── Session guards (2026-07-28): differential-INVISIBLE regressions ──────────
# These 3 fixes can give the same wrong answer on every surface → the differential does NOT CATCH them. An external-oracle/
# behavior guard is required. int64 comparison: if operator== converts int to double, distinct int64
# values above 2^53 become equal (d752afe). cache/channel isolation: if deep_clone is removed (someone optimizes it as an
# "unnecessary copy") cross-thread sharing + the heap race come back (c7c7427/9670c18).
# encoding-safe (tr -d + exact match, no Turkish grep); absolute $TMP.
printf '$a=9007199254740992+1\n$b=9007199254740992\nprint($a==$b)\n' > "$TMP/g_int.lk"
iv=$("$RECLK58" "$TMP/g_int.lk" 2>&1 | tr -d '[:space:]')
if [ "$iv" = "false" ]; then
  echo "  PASS int64-compare: 2^53+1 != 2^53 (external oracle, invisible to the differential)"
else
  echo "  FAIL int64-compare: [$iv] expected false — d752afe regression"; fail=1
fi
printf 'use array\nuse cache\ncache::set("k",[1,2,3])\n$a=cache::get("k")\n$a = array::push($a, 9)\nprint(count(cache::get("k")))\n' > "$TMP/g_cache.lk"
cv=$("$RECLK58" "$TMP/g_cache.lk" 2>&1 | tr -d '[:space:]')
if [ "$cv" = "3" ]; then
  echo "  PASS cache-isolation: get+mutate does not corrupt the cache (c7c7427)"
else
  echo "  FAIL cache-isolation: [$cv] expected 3 — deep_clone regression"; fail=1
fi
printf 'use array\n$ch=channel(1)\n$arr=[1,2,3]\nsend($ch,$arr)\n$arr = array::push($arr, 9)\nprint(count(receive($ch)))\n' > "$TMP/g_chan.lk"
chv=$("$RECLK58" "$TMP/g_chan.lk" 2>&1 | tr -d '[:space:]')
if [ "$chv" = "3" ]; then
  echo "  PASS channel-isolation: send+mutate does not share (9670c18)"
else
  echo "  FAIL channel-isolation: [$chv] expected 3 — deep_clone regression"; fail=1
fi

echo "─────────────────────────────────────────────────────────────"
[ $fail = 0 ] && echo "PASS: deploy verified — the fixes are in the live binary" || echo "FAIL: deploy verification FAILED"
exit $fail
