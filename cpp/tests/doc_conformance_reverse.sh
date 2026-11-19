#!/usr/bin/env bash
# REVERSE-direction doc conformance guard (code → docs). The existing conformance guard checks docs→code
# (is the mod::fn written in docs really there — the session::has class). This is its missing half: is EVERY
# namespaced builtin in builtin_names() documented in docs/index.html? If not it must be documented or excluded (with a reason).
# So a new builtin cannot pass CI without being documented. Positive check: delete a function from docs → RED.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
DOCS="$ROOT/docs/look.codlook.com/docs.html"
fail=0

# JUSTIFIED EXCLUSIONS (NOT known_fail — each is documented in docs in ANOTHER form):
#  - validator::* : documented as "Rule strings" (not functions, rule names passed to check())
#  - template::extends/block/include : documented as {#extends}/{#block}/{#include} SYNTAX
# If a new builtin is added to this list a REASON is required (as a comment), otherwise the list turns into known_fail.
EXCLUDE="validator::required validator::email validator::integer validator::numeric validator::min validator::max template::extends template::block template::include"

# extract the namespaced names from builtin_names() (mod::fn)
names=$(grep -oE '"[a-z_]+::[a-z_0-9]+"' "$BUILTINS" | tr -d '"' | sort -u)
undoc=""
for n in $names; do
  case " $EXCLUDE " in *" $n "*) continue;; esac   # justified exclusion
  grep -qF "$n" "$DOCS" || undoc="$undoc $n"
done

if [ -n "$undoc" ]; then
  echo "  FAIL: builtins exist but are MISSING from docs (document them, or add them to EXCLUDE with a reason):"
  for u in $undoc; do echo "    $u"; done
  fail=1
else
  echo "  OK   all namespaced builtins are documented (or excluded with a reason)"
fi

# BARE (non-namespaced) builtins — were a BLIND SPOT: args() could be added undocumented and pass the guard
# because the scan above only checked the "mod::fn" form. Check these too.
# Some bare names are INTENTIONALLY undocumented: backward-compatibility aliases
# whose canonical form is documented (json_encode→json::encode, strlen→string::len...). They are in BARE_EXCLUDE.
BARE_EXCLUDE="json_encode json_decode strlen strtolower strtoupper"
bare=$(sed -n '/const std::vector<std::string>& builtin_names/,/return NAMES;/p' "$BUILTINS" \
  | grep -oE '"[a-z_][a-z0-9_]*"' | tr -d '"' | grep -vE '::' | sort -u)
undoc_bare=""
for n in $bare; do
  case " $BARE_EXCLUDE " in *" $n "*) continue;; esac
  grep -qw "$n" "$DOCS" || undoc_bare="$undoc_bare $n"
done
if [ -n "$undoc_bare" ]; then
  echo "  FAIL: bare builtins exist but are MISSING from docs (document them, or add them to BARE_EXCLUDE with a reason):"
  for u in $undoc_bare; do echo "    $u"; done
  fail=1
else
  echo "  OK   all bare builtins are documented (or excluded with a reason)"
fi

# POSITIVE CHECK: when a documented function (session::regenerate) is temporarily removed from docs the guard must go RED.
if grep -qF "session::regenerate" "$DOCS"; then
  echo "  OK   positive-control reference present (session::regenerate is documented)"
else
  echo "  FAIL positive control: session::regenerate is missing from docs — guard calibration is broken"; fail=1
fi

[ $fail = 0 ] && echo "PASS: reverse-direction doc conformance (code->docs)" || echo "FAIL: reverse-direction doc conformance"
exit $fail
