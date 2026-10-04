#!/usr/bin/env bash
# The runtime's own JSON error bodies carry the English key "error" (the same key
# response::error() uses). The older Turkish key "hata" stays beside it for the whole 1.x
# line, because existing clients read it; it goes away in 2.0.
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
    # KNOWN DIVERGENCE (older than this test, not decided yet): for an unmatched route the VM
    # path answers with the plain text "404 Not Found", the interpreter path with JSON. The
    # default (VM) body is left alone here; both are pinned so a change is a deliberate one.
    if [ "$mode" = "LOOK_BYTECODE=1" ]; then
        [ "$body" = "404 Not Found" ] && ok "[$mode] 404 body is the plain text (pinned)" || bad "[$mode] 404 body changed: $body"
    else
        case "$body" in *'"error":"Endpoint not found"'*'"hata":"Endpoint not found"'*) ok "[$mode] 404 body has error and hata";; *) bad "[$mode] 404 body: $body";; esac
    fi
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
