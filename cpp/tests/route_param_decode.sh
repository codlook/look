#!/bin/bash
# Regression guard: route (path) parameters must be URL-decoded, like query params.
# Found by a dogfood — /p/%C3%BC gave "%C3%BC" (raw) instead of "ü": query params were
# decoded but path params were not (the sibling-path-lagged-behind class). Both the function
# argument and request::param() must decode; '+' stays literal in a path (not a space).
# Runs both dispatch engines (bytecode VM default, tree-walk via LOOK_BYTECODE=0).
set -u
FCGI="${1:?usage: route_param_decode.sh <lk-fcgi>}"
DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=7533
APP="$(mktemp --suffix=.lk)"
cat > "$APP" <<'LK'
route("GET", "/p/{v}", function($v) { print(json::encode(["arg" => $v, "param" => request::param("v")])) })
LK

fail=0
check() {  # label  expected
  local got; got="$(curl -s -m 5 "http://127.0.0.1:$PORT/p/$1")"
  if [ "$got" != "$2" ]; then echo "  FAIL /p/$1 -> $got  (expected $2)"; fail=1
  else echo "  ok   /p/$1 -> $got"; fi
}

for eng in "VM:" "tree-walk:LOOK_BYTECODE=0"; do
  name="${eng%%:*}"; env="${eng#*:}"
  echo "== $name =="
  env $env "$FCGI" --mode http --port $PORT "$APP" >/dev/null 2>&1 &
  srv=$!; sleep 1.5
  check "%C3%BC"       '{"arg":"ü","param":"ü"}'       # UTF-8 ü decoded
  check "a%20b"        '{"arg":"a b","param":"a b"}'    # %20 -> space
  check "a+b"          '{"arg":"a+b","param":"a+b"}'    # '+' literal in a path
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
done
rm -f "$APP"
[ $fail = 0 ] && echo "PASS: route params URL-decoded in both engines" || { echo "FAIL: route-param decoding broken"; exit 1; }
