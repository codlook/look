#!/usr/bin/env bash
# docs-conformance — dokümandaki mod::fn API'si ile çekirdeğin (builtin_names()) TEK
# programatik kaynağını karşılaştırır. İki-liste diff, çalıştırma yok.
#
# NEDEN: `session::has` bug'ı (dogfooding #1) — docs bir fonksiyonu VAAT ediyordu ama
# builtin_names()'te yoktu → dokümanı takip eden kullanıcı 500 alıyordu. Differential
# göremez (iki motor da eksik), regression göremez (kimse çağırmıyor). Bir örnek =
# bir SINIF: "docs var, çekirdek yok". Bu tarama tüm sınıfı tek seferde kapatır.
#
# YÖN 1 (docs var, builtin_names yok) = KULLANICI 500 RİSKİ → allowlist dışı her giriş HATA.
# YÖN 2 (builtin_names var, docs yok) = belgesiz yüzey → yalnız bilgilendirme (fail etmez).
#
# Kullanım: bash cpp/tests/docs_conformance.sh   (repo kökünden veya cpp/'den)
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
DOCS="$ROOT/docs/index.html"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
[ -f "$BUILTINS" ] || { echo "builtins.cpp yok: $BUILTINS"; exit 2; }
# docs/ REPO'DA DEĞİL (gitignored — website kaynağı, ayrı deploy). CI checkout'unda yok.
# Bu durumda SAHTE-YEŞİL vermemek için AÇIKÇA "ATLANDI" de (exit 0) — "geçti" DEME.
# Kalıcı çözüm (docs'u commit et / fixture snapshot) kullanıcı kararı; yerelde tam çalışır.
if [ ! -f "$DOCS" ]; then
  echo "ATLANDI — docs/index.html repo'da yok (gitignored); guard yalnız YEREL çalışır."
  echo "  Kalıcı CI için: docs'u commit et VEYA fixture snapshot'a bağla (karar bekliyor)."
  exit 0
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Docs'taki mod::fn (tag'leri BOŞLUKLA sil → prose yapışmasını önle; rakamlara izin ver)
sed 's/<[^>]*>/ /g' "$DOCS" \
  | grep -oE '[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*' | sort -u > "$TMP/docs.txt"
# builtin_names()'teki mod::fn (tek programatik kaynak)
grep -oE '"[a-z_][a-z0-9_]*::[a-z_][a-z0-9_]*"' "$BUILTINS" | tr -d '"' | sort -u > "$TMP/vm.txt"

# ── Allowlist: docs'ta görünen ama builtin_names()'te OLMAMASI DOĞRU olanlar ──
# Her giriş gerekçeli; yeni bir gerçek boşluk (session::has gibi) buraya GİRMEDEN fail eder.
cat > "$TMP/allow.txt" <<'ALLOW'
# C++ kod örnekleri (LOOK API değil)
std::any
std::chrono
std::deque
std::localtime
std::map
std::mismatch
std::mutex
std::sto
std::stoll
std::string
std::thread
std::unordered_map
pp::generation
pp::program
pp::setup_out
condition_variable::wait_until
# Docs prose: bilinçli YOKLUĞU anlatıyor (fonksiyon değil)
cache::remember
db::pool
route::run
str::length
# Yalnız `lk test` bağlamı (web builtin değil)
assert::eq
# Interpreter'da ÖZEL-DURUM (scope-resolution; builtin_names dışı ama çalışır) — ws/sse/timer/jobs
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
grep -vE '^#' "$TMP/allow.txt" | sed '/^$/d' | sort -u > "$TMP/allow_clean.txt"

# YÖN 1 — docs var, vm yok, allowlist'te de yok → HATA
comm -23 "$TMP/docs.txt" "$TMP/vm.txt" | sort -u > "$TMP/docs_only.txt"
comm -23 "$TMP/docs_only.txt" "$TMP/allow_clean.txt" > "$TMP/violations.txt"

# YÖN 2 — vm var, docs yok → bilgilendirme
comm -13 "$TMP/docs.txt" "$TMP/vm.txt" > "$TMP/vm_only.txt"

echo "=== YÖN 2 (bilgilendirme) — builtin_names'te VAR, docs'ta YOK ($(wc -l < "$TMP/vm_only.txt")) ==="
echo "  (belgesiz yüzey — fail etmez; zamanla docs'a eklenmeli)"
sed 's/^/  /' "$TMP/vm_only.txt"
echo ""

if [ -s "$TMP/violations.txt" ]; then
  echo "=== HATA: docs'ta VAAT edilen ama builtin_names()'te OLMAYAN (kullanıcı 500 alır) ==="
  sed 's/^/  ✗ /' "$TMP/violations.txt"
  echo ""
  echo "Fix: ya çekirdeğe ekle (session::has gibi) ya da docs'tan kaldır."
  echo "Gerçekten kasıtlı-yok ise gerekçesiyle allowlist'e ekle."
  exit 1
fi
echo "=== docs-conformance GEÇTİ: docs'taki her mod::fn çekirdekte karşılanıyor ==="
exit 0
