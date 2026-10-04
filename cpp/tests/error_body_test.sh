#!/usr/bin/env bash
# LOOK 2: an unmatched route answers the same on both engines, and an application can
# replace that answer with route("404", ...).
# Also: 'use timer' (a built-in namespace, not a module) must say so, in English, instead
# of telling the author to install a module that does not exist.
# Usage: error_body_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7721}"; fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

echo 'route("GET", "/ok", fn() => response::text("ok"))' > "$TMP/app.lk"
for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do [ "$(curl -s -m 1 "localhost:$PORT/ok")" = ok ] && break; sleep 0.1; done
    body="$(curl -s -m 3 "localhost:$PORT/no-such-route")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    # LOOK 2: one answer for an unmatched route on both engines — the plain text the VM has
    # always sent. (In 1.x the interpreter path sent JSON instead; that divergence is gone.)
    [ "$body" = "404 Not Found" ] && ok "[$mode] unmatched route answers '404 Not Found'" || bad "[$mode] 404 body: $body"
    # An application that wants its own body defines route("404", ...), on either engine.
done

printf 'route("GET", "/ok", fn() => response::text("ok"))\nroute("404", fn() => response::text("custom-404"))\n' > "$TMP/app.lk"
for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do [ "$(curl -s -m 1 "localhost:$PORT/ok")" = ok ] && break; sleep 0.1; done
    body="$(curl -s -m 3 "localhost:$PORT/no-such-route")"; code="$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/no-such-route")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$body" = "custom-404" ] && [ "$code" = 404 ] && ok "[$mode] route(\"404\") replaces the body, status stays 404" || bad "[$mode] custom 404: [$code] $body"
done

printf 'route("GET", "/ok", fn() => response::text("ok"))\nroute("404", fn() => response::text("custom-404"))\n' > "$TMP/app.lk"
for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do [ "$(curl -s -m 1 "localhost:$PORT/ok")" = ok ] && break; sleep 0.1; done
    body="$(curl -s -m 3 "localhost:$PORT/no-such-route")"; code="$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/no-such-route")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$body" = "custom-404" ] && [ "$code" = 404 ] && ok "[$mode] route(\"404\") replaces the body, status stays 404" || bad "[$mode] custom 404: [$code] $body"
done

for name in timer ws sse; do
    printf 'use %s\nprint("x")\n' "$name" > "$TMP/u.lk"
    out="$(LOOK_CLI_VM=0 "$LK" "$TMP/u.lk" 2>&1)"
    case "$out" in *"'$name' is built in and is not a module: remove the 'use $name' line"*) ok "use $name explains that it is built in";; *) bad "use $name: $(echo "$out" | tr '\n' ' ' | cut -c1-160)";; esac
done
printf 'use nosuchpkg\n' > "$TMP/u.lk"
out="$(LOOK_CLI_VM=0 "$LK" "$TMP/u.lk" 2>&1)"
case "$out" in *"Unknown module: 'nosuchpkg'. If it is a package, install it with: lk module install nosuchpkg"*) ok "unknown module message is English";; *) bad "unknown module: $(echo "$out" | tr '\n' ' ' | cut -c1-160)";; esac
[ $fail = 0 ] && echo "PASS: error bodies and module messages" || { echo "FAIL: error bodies / module messages"; exit 1; }
