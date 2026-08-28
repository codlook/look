#!/bin/bash
# Guard: the session cookie's Secure flag is set ONLY when the connection is really HTTPS.
# Found by a dogfood — it was hardcoded Secure, so sessions silently never worked over plain
# HTTP (the documented localhost quickstart): the browser withholds a Secure cookie on http://.
# Secure only protects under TLS, so gating it on HTTPS loses no protection and fixes local dev.
# HTTPS is detected from X-Forwarded-Proto, but ONLY from a trusted proxy (LOOK_TRUSTED_PROXY) —
# a client must not be able to forge the header. Runs both dispatch engines.
set -u
FCGI="${1:?usage: session_secure.sh <lk-fcgi>}"
PORT=7550
APP="$(mktemp --suffix=.lk)"
cat > "$APP" <<'LK'
use session
route("GET", "/set", function() { session::start(); session::set("u", "ali"); print("ok") })
route("GET", "/get", function() { session::start(); print(json::encode(["u" => session::get("u")])) })
LK

fail=0
has_secure() { echo "$1" | grep -qi '; *Secure'; }

for eng in "VM:" "tree-walk:LOOK_BYTECODE=0"; do
  name="${eng%%:*}"; env="${eng#*:}"
  echo "== $name =="

  # (a) plain HTTP: no Secure, and the session must round-trip
  env $env "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 & s=$!; sleep 1.2
  sc="$(curl -s -m5 -i -c /tmp/_ck http://127.0.0.1:$PORT/set | grep -i set-cookie)"
  rt="$(curl -s -m5 -b /tmp/_ck http://127.0.0.1:$PORT/get)"
  if has_secure "$sc"; then echo "  FAIL (a) plain HTTP set Secure: $sc"; fail=1; else echo "  ok   (a) HTTP: no Secure"; fi
  if [ "$rt" = '{"u":"ali"}' ]; then echo "  ok   (a) HTTP: session round-trips"; else echo "  FAIL (a) round-trip: $rt"; fail=1; fi
  kill $s 2>/dev/null; wait $s 2>/dev/null

  # (b) HTTPS via a TRUSTED proxy: Secure present
  env $env LOOK_TRUSTED_PROXY=127.0.0.1 "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 & s=$!; sleep 1.2
  sc="$(curl -s -m5 -i -H 'X-Forwarded-Proto: https' http://127.0.0.1:$PORT/set | grep -i set-cookie)"
  if has_secure "$sc"; then echo "  ok   (b) HTTPS (trusted): Secure set"; else echo "  FAIL (b) HTTPS: no Secure: $sc"; fail=1; fi
  kill $s 2>/dev/null; wait $s 2>/dev/null

  # (c) forged X-Forwarded-Proto with NO trusted proxy: header ignored, no Secure
  env $env "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 & s=$!; sleep 1.2
  sc="$(curl -s -m5 -i -H 'X-Forwarded-Proto: https' http://127.0.0.1:$PORT/set | grep -i set-cookie)"
  if has_secure "$sc"; then echo "  FAIL (c) forged XFP set Secure (untrusted): $sc"; fail=1; else echo "  ok   (c) forged XFP ignored"; fi
  kill $s 2>/dev/null; wait $s 2>/dev/null
done

# Explicit override (engine-independent) + fail-loud warning.
echo "== override + warning =="
# (d) LOOK_SESSION_SECURE=1 forces Secure even over plain HTTP
LOOK_SESSION_SECURE=1 "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 & s=$!; sleep 1.2
sc="$(curl -s -m5 -i http://127.0.0.1:$PORT/set | grep -i set-cookie)"
if has_secure "$sc"; then echo "  ok   (d) LOOK_SESSION_SECURE=1 forces Secure"; else echo "  FAIL (d) =1 did not force Secure: $sc"; fail=1; fi
kill $s 2>/dev/null; wait $s 2>/dev/null

# (e) LOOK_SESSION_SECURE=0 forces no Secure even with an HTTPS signal
LOOK_SESSION_SECURE=0 LOOK_TRUSTED_PROXY=127.0.0.1 "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 & s=$!; sleep 1.2
sc="$(curl -s -m5 -i -H 'X-Forwarded-Proto: https' http://127.0.0.1:$PORT/set | grep -i set-cookie)"
if has_secure "$sc"; then echo "  FAIL (e) =0 still set Secure: $sc"; fail=1; else echo "  ok   (e) LOOK_SESSION_SECURE=0 forces no Secure"; fi
kill $s 2>/dev/null; wait $s 2>/dev/null

# (f) fail-loud: a session cookie without Secure and no HTTPS signal must WARN (not silent)
LOG="$(mktemp)"
"$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>"$LOG" & s=$!; sleep 1.2
curl -s -m5 http://127.0.0.1:$PORT/set >/dev/null
kill $s 2>/dev/null; wait $s 2>/dev/null
if grep -qi 'WITHOUT Secure' "$LOG"; then echo "  ok   (f) warns when shipping a non-Secure session cookie"; else echo "  FAIL (f) no warning — silent security downgrade"; fail=1; fi
rm -f "$LOG"

rm -f "$APP" /tmp/_ck
[ $fail = 0 ] && echo "PASS: session Secure is HTTPS-only, proxy-gated, overridable, and fail-loud" || { echo "FAIL"; exit 1; }
