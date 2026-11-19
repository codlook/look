#!/usr/bin/env bash
# docs-conformance — is the mod::fn API in the documentation provided by the core (builtin_names)?
#
# WHY: the `session::has` bug (dogfooding #1) — docs PROMISED a function, the core
# did not provide it → a user following the docs got a 500. Differential cannot see it (both engines lack it),
# regression cannot see it (nobody calls it). One example = one CLASS; this scan closes the class.
#
# TWO-SIDED CLOSURE (docs/ is NOT IN THE REPO — a gitignored website source, absent in CI):
#   • Fixture (cpp/tests/docs_api.txt) = the committed SHADOW of docs that enters CI.
#   • LOCAL (docs present):  regenerate the fixture from docs → COMPARE with the committed one.
#     If different FAIL: "docs changed, fixture stale" (a stale fixture = false green).
#   • ALWAYS (incl. CI): fixture − allowlist ⊆ builtin_names()  → the real guard.
#
# Update the fixture:  bash cpp/tests/docs_conformance.sh --update   (docs must be accessible)
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
DOCS="$ROOT/docs/look.codlook.com/docs.html"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
FIXTURE="$DIR/docs_api.txt"
[ -f "$BUILTINS" ] || { echo "builtins.cpp yok: $BUILTINS"; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Generate the documented mod::fn from docs (a SINGLE place — generation and staleness check use the same command).
# Delete tags WITH A SPACE (prevent prose from sticking together); filter out C++ example noise (std/pp/...).
gen_from_docs() {
  sed 's/<[^>]*>/ /g' "$DOCS" \
    | grep -oE '[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*' \
    | grep -vE '^(std|pp|condition_variable)::' | sort -u
}

# --update: rewrite the fixture from docs
if [ "${1:-}" = "--update" ]; then
  [ -f "$DOCS" ] || { echo "--update için docs/index.html gerekli (bulunamadı: $DOCS)"; exit 2; }
  gen_from_docs > "$FIXTURE"
  echo "Fixture güncellendi: $FIXTURE ($(wc -l < "$FIXTURE") giriş)"
  exit 0
fi

[ -f "$FIXTURE" ] || { echo "fixture yok: $FIXTURE (önce --update ile üret)"; exit 2; }

# ── Staleness check (only if docs is accessible — locally) ──
if [ -f "$DOCS" ]; then
  gen_from_docs > "$TMP/fresh.txt"
  if ! diff -q "$FIXTURE" "$TMP/fresh.txt" >/dev/null; then
    echo "=== HATA: fixture BAYAT — docs değişti, docs_api.txt güncellenmemiş ==="
    echo "  Fark (< fixture / > docs):"; diff "$FIXTURE" "$TMP/fresh.txt" | sed 's/^/    /'
    echo "  Fix: bash cpp/tests/docs_conformance.sh --update  (+ allowlist'i gözden geçir)"
    exit 1
  fi
  echo "(bayatlama kontrolü GEÇTİ — fixture docs ile senkron)"
else
  echo "(docs/ yok — bayatlama kontrolü atlandı; fixture↔builtin_names asıl guard koşuyor)"
fi

# mod::fn in builtin_names() (the single programmatic source)
grep -oE '"[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*"' "$BUILTINS" | tr -d '"' | sort -u > "$TMP/vm.txt"

# ── Allowlist: documented but CORRECTLY ABSENT from builtin_names() (with reasons) ──
grep -vE '^\s*#|^\s*$' <<'ALLOW' | sort -u > "$TMP/allow.txt"
# Docs prose: describes the deliberate ABSENCE (not a function)
cache::remember
db::pool
route::run
str::length
# `lk test` context only (not a web builtin)
assert::eq
# SPECIAL CASE in the interpreter (scope-resolution; outside builtin_names but works)
ws::on
ws::send
ws::close
ws::broadcast
ws::clients
sse::on
sse::send
sse::close
sse::clients
timer::after
timer::every
timer::cancel
jobs::run
jobs::worker
# Regex artifact: "type::is_*" in docs (the predicate family) gets cut to "type::is_" by `::[a-z_0-9]+` —
# not a real function (type::is_int/is_string/... are documented and builtin separately).
type::is_
ALLOW

# Vacuous-green guard (Rule 2): an empty/truncated fixture PASSES without testing anything —
# since docs/ is gitignored in CI the staleness check (line 44) is skipped, only the
# membership check below remains; if the fixture is empty violations is empty too → exit 0 (false green).
# MEASURED (2026-08-11): no docs + empty fixture → "PASSED" without testing anything.
# Pin the fixture line count to a floor (real ~239); truncation/bad merge fails loudly.
FIXTURE_MIN=200
fixture_lines=$(grep -c . "$FIXTURE")
if [ "$fixture_lines" -lt "$FIXTURE_MIN" ]; then
  echo "=== HATA: docs_api.txt yalnız $fixture_lines satır (min $FIXTURE_MIN) — boş/kesik fixture, vacuous-green riski ==="
  exit 1
fi

# DIRECTION 1 — in fixture, not in vm, not in allowlist either → ERROR (user 500 risk)
comm -23 "$FIXTURE" "$TMP/vm.txt" > "$TMP/docs_only.txt"
comm -23 "$TMP/docs_only.txt" "$TMP/allow.txt" > "$TMP/violations.txt"
# DIRECTION 2 — in vm, not in fixture → informational (undocumented surface)
comm -13 "$FIXTURE" "$TMP/vm.txt" > "$TMP/vm_only.txt"

echo "=== YÖN 2 (bilgilendirme) — builtin_names'te VAR, docs'ta YOK ($(wc -l < "$TMP/vm_only.txt")) ==="
sed 's/^/  /' "$TMP/vm_only.txt"
echo ""
if [ -s "$TMP/violations.txt" ]; then
  echo "=== HATA: docs'ta VAAT edilen ama builtin_names()'te OLMAYAN (kullanıcı 500 alır) ==="
  sed 's/^/  x /' "$TMP/violations.txt"
  echo "Fix: çekirdeğe ekle (session::has gibi), docs'tan kaldır, ya da gerekçeyle allowlist'e."
  exit 1
fi
echo "=== docs-conformance GEÇTİ: fixture'daki her mod::fn çekirdekte karşılanıyor ==="
exit 0
