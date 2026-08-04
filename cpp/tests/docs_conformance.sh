#!/usr/bin/env bash
# docs-conformance — dokümandaki mod::fn API'si çekirdekte (builtin_names) karşılanıyor mu?
#
# NEDEN: `session::has` bug'ı (dogfooding #1) — docs bir fonksiyonu VAAT ediyordu, çekirdek
# vermiyordu → dokümanı takip eden kullanıcı 500. Differential göremez (iki motor da eksik),
# regression göremez (kimse çağırmıyor). Bir örnek = bir SINIF; bu tarama sınıfı kapatır.
#
# İKİ TARAFLI KAPALILIK (docs/ REPO'DA DEĞİL — gitignored website kaynağı, CI'da yok):
#   • Fixture (cpp/tests/docs_api.txt) = docs'un CI'a giren commit'li GÖLGESİ.
#   • YEREL (docs var):  fixture'ı docs'tan yeniden üret → commit'liyle KARŞILAŞTIR.
#     Farklıysa FAIL: "docs değişti, fixture bayat" (bayat fixture = sahte yeşil).
#   • HER ZAMAN (CI dahil): fixture − allowlist ⊆ builtin_names()  → asıl guard.
#
# Fixture güncelle:  bash cpp/tests/docs_conformance.sh --update   (docs erişilebilir olmalı)
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
DOCS="$ROOT/docs/index.html"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
FIXTURE="$DIR/docs_api.txt"
[ -f "$BUILTINS" ] || { echo "builtins.cpp yok: $BUILTINS"; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Docs'tan belgeli mod::fn'leri üret (TEK yer — üretim ve bayatlama-kontrolü aynı komut).
# Tag'leri BOŞLUKLA sil (prose yapışmasını önle); C++ örnek gürültüsünü (std/pp/...) ayıkla.
gen_from_docs() {
  sed 's/<[^>]*>/ /g' "$DOCS" \
    | grep -oE '[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*' \
    | grep -vE '^(std|pp|condition_variable)::' | sort -u
}

# --update: fixture'ı docs'tan yeniden yaz
if [ "${1:-}" = "--update" ]; then
  [ -f "$DOCS" ] || { echo "--update için docs/index.html gerekli (bulunamadı: $DOCS)"; exit 2; }
  gen_from_docs > "$FIXTURE"
  echo "Fixture güncellendi: $FIXTURE ($(wc -l < "$FIXTURE") giriş)"
  exit 0
fi

[ -f "$FIXTURE" ] || { echo "fixture yok: $FIXTURE (önce --update ile üret)"; exit 2; }

# ── Bayatlama kontrolü (yalnız docs erişilebilirse — yerelde) ──
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

# builtin_names()'teki mod::fn (tek programatik kaynak)
grep -oE '"[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*"' "$BUILTINS" | tr -d '"' | sort -u > "$TMP/vm.txt"

# ── Allowlist: belgeli ama builtin_names()'te OLMAMASI DOĞRU olanlar (gerekçeli) ──
grep -vE '^\s*#|^\s*$' <<'ALLOW' | sort -u > "$TMP/allow.txt"
# Docs prose: bilinçli YOKLUĞU anlatıyor (fonksiyon değil)
cache::remember
db::pool
route::run
str::length
# Yalnız `lk test` bağlamı (web builtin değil)
assert::eq
# Interpreter'da ÖZEL-DURUM (scope-resolution; builtin_names dışı ama çalışır)
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
ALLOW

# YÖN 1 — fixture var, vm yok, allowlist'te de yok → HATA (kullanıcı 500 riski)
comm -23 "$FIXTURE" "$TMP/vm.txt" > "$TMP/docs_only.txt"
comm -23 "$TMP/docs_only.txt" "$TMP/allow.txt" > "$TMP/violations.txt"
# YÖN 2 — vm var, fixture yok → bilgilendirme (belgesiz yüzey)
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
