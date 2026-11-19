#!/usr/bin/env bash
# route::group() guard — Go/chi style prefix + middleware inheritance + nesting. Against lk-fcgi --mode http.
# Self-discriminating:
#   - prefix inheritance: /admin/solo=200 BUT bare /solo=404 (if the prefix were not applied /solo would open)
#   - middleware inheritance: /admin/users no-token=403 (if the mw were not inherited it would return 200)
#   - nested: /admin/deep/x inherits the prefix stack + the outer group's mw
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7715}"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/route_group_app.lk"
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }
fail=0

run_engine() {
  local label="$1" port="$2" env="$3"
  local srv=""
  env $env "$FCGI" --mode http --port "$port" "$APP" >/dev/null 2>&1 &
  srv=$!
  for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$port/public" 2>/dev/null && break; sleep 0.2; done
  local B="http://127.0.0.1:$port"
  code(){ curl -s -o /dev/null -w "%{http_code}" ${2:-} "$B$1"; }
  chk(){ if [ "$2" = "$3" ]; then echo "  OK   [$label] $1 ($2)"; else echo "  FAIL [$label] $1: $2 beklenen $3"; fail=1; fi; }
  chk "/public"                "$(code /public)"                       "200"
  chk "/admin/users no-tok"    "$(code /admin/users)"                  "403"   # mw inherited
  chk "/admin/users tok"       "$(code /admin/users '-H X-Token:sekret')" "200"
  chk "/admin/solo tok"        "$(code /admin/solo '-H X-Token:sekret')"  "200"  # prefix applied
  chk "/solo (bare) 404"       "$(code /solo)"                         "404"   # would be open if there were NO prefix
  chk "/admin/deep/x no-tok"   "$(code /admin/deep/x)"                 "403"   # nested mw inherited
  chk "/admin/deep/x tok"      "$(code /admin/deep/x '-H X-Token:sekret')" "200" # nested prefix stack
  kill "$srv" 2>/dev/null; sleep 0.3; kill -9 "$srv" 2>/dev/null
}

run_engine "VM"     "$PORT"        ""

[ $fail = 0 ] && echo "PASS: route::group (prefix+mw kalıtımı + nested)" || echo "FAIL: route::group"
exit $fail
