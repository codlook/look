#!/usr/bin/env bash
# installer:: look.lock içerik özeti (h1) + atomik kurulum — AĞSIZ.
#
# Dikiş: LOOK_INSTALL_ARCHIVE_DIR → arşiv GitHub yerine <dir>/<user>/<repo>/<ref>.zip'ten okunur,
# commit sha'sı <ref>.sha'dan (GitHub Content-Disposition taklidi). ZIP'ler python3 ile, GitHub
# zipball düzeninde ("repo-sha/" öneki) üretilir.
#
# Kilitlenenler:
#  1) ilk kurulum özeti look.lock'a yazar ("ref@sha h1:...")
#  2) aynı commit + aynı içerik (FARKLI ZIP baytları) → kurulur, "checksum verified"
#  3) aynı commit + değişmiş içerik → net hata, eski paket ve look.lock dokunulmamış
#  4) özetsiz eski kilit satırı → kurulur, özet eklenir (trust on first use), --locked dahil
#  5) yeni commit'e geçiş → yeni özet
#  6) bozuk/yarım arşiv (CRC hatası, ZIP değil) → eski paket sağlam, geçici dizin kalmaz
#  7) lk module install: aynı atomiklik (modüllerin kilidi yok)
set -u
LK="${1:-./build/lk}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"
command -v python3 >/dev/null || { echo "FAIL: python3 required"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export LOOK_INSTALL_ARCHIVE_DIR="$TMP/arch"
export HOME="$TMP/home"
mkdir -p "$TMP/proj" "$HOME" "$TMP/arch/acme/widget"
cd "$TMP/proj" || exit 1
A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
AR="$TMP/arch/acme/widget"
pass=0; fail=0
ok()  { echo "  OK   $1"; pass=$((pass+1)); }
bad() { echo "  FAIL $1"; fail=$((fail+1)); }

# mkzip <out> <sha> <mode:deflate|stored|reversed> <path=content>...
mkzip() {
  python3 -I - "$@" <<'PY'
import sys, zipfile
out, sha, mode = sys.argv[1], sys.argv[2], sys.argv[3]
files = [a.split("=", 1) for a in sys.argv[4:]]
if mode == "reversed": files.reverse()
comp = zipfile.ZIP_STORED if mode == "stored" else zipfile.ZIP_DEFLATED
with zipfile.ZipFile(out, "w", comp) as z:
    z.writestr("widget-%s/" % sha[:7], "")
    for p, c in files:
        z.writestr("widget-%s/%s" % (sha[:7], p), c)
PY
}
# corrupt <zip> <marker>: veride marker'ın bir baytını değiştir (CRC uyuşmazlığı)
corrupt() {
  python3 -I - "$1" "$2" <<'PY'
import sys
p, m = sys.argv[1], sys.argv[2].encode()
d = bytearray(open(p, "rb").read())
i = d.find(m)
assert i >= 0, "marker not found"
d[i] ^= 0x20
open(p, "wb").write(bytes(d))
PY
}
lockline() { grep '"acme/widget"' look.lock 2>/dev/null; }
lockhash() { lockline | grep -o 'h1:[A-Za-z0-9+/=]*'; }
leftovers() { ls -A "$1" 2>/dev/null | grep -c '\.look-' ; }

# ── 1) ilk kurulum özeti kaydeder ──────────────────────────────────────────────
mkzip "$AR/main.zip" "$A" deflate "widget.lk=v1" "lib/util.lk=util-v1"
echo "$A" > "$AR/main.sha"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
H1=$(lockhash)
if [ $rc = 0 ] && [ "$(cat pkg/acme/widget/widget.lk 2>/dev/null)" = v1 ] \
   && lockline | grep -q "\"main@$A h1:" && [ -n "$H1" ]; then ok "first install records the checksum ($H1)"
else bad "first install: rc=$rc lock=[$(lockline)] out=[$out]"; fi

# ── 2) aynı commit, aynı içerik, farklı ZIP baytları → kurulur ─────────────────
mkzip "$AR/$A.zip" "$A" stored "widget.lk=v1" "lib/util.lk=util-v1"
mkzip "$AR/$A.zip.rev" "$A" reversed "widget.lk=v1" "lib/util.lk=util-v1"
if cmp -s "$AR/$A.zip" "$AR/main.zip"; then bad "test setup: ZIP bytes should differ"; fi
out=$("$LK" install 2>&1); rc=$?
if [ $rc = 0 ] && echo "$out" | grep -q "checksum verified" && lockline | grep -q "\"main@$A $H1\""; then
  ok "lk install (from look.lock): matching content installs, checksum verified"
else bad "matching content: rc=$rc out=[$out]"; fi
mv "$AR/$A.zip.rev" "$AR/$A.zip"
out=$("$LK" install --locked 2>&1); rc=$?
if [ $rc = 0 ] && echo "$out" | grep -q "checksum verified"; then ok "--locked: matching content (other entry order) installs"
else bad "--locked matching: rc=$rc out=[$out]"; fi

# ── 3) aynı commit, değişmiş içerik → reddedilir, hiçbir şey değişmez ──────────
cp look.lock "$TMP/lock.before"
mkzip "$AR/$A.zip" "$A" deflate "widget.lk=EVIL" "lib/util.lk=util-v1"
for args in "install" "install --locked"; do
  out=$("$LK" $args 2>&1); rc=$?
  if [ $rc != 0 ] && echo "$out" | grep -q "checksum mismatch for acme/widget at commit $A" \
     && echo "$out" | grep -q "look.lock:  $H1" && echo "$out" | grep -q "downloaded: h1:" \
     && echo "$out" | grep -q "nothing was installed" \
     && [ "$(cat pkg/acme/widget/widget.lk)" = v1 ] && cmp -s look.lock "$TMP/lock.before" \
     && [ "$(leftovers pkg/acme)" = 0 ]; then ok "tampered content refused (lk $args); old package and look.lock untouched"
  else bad "tampered (lk $args): rc=$rc pkg=[$(cat pkg/acme/widget/widget.lk)] out=[$out]"; fi
done
# lk install github.com/acme/widget çözümü main.zip'ten gelir; onu da tahrif et
cp "$AR/main.zip" "$TMP/main.good"
cp "$AR/$A.zip" "$AR/main.zip"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
if [ $rc != 0 ] && echo "$out" | grep -q "checksum mismatch" && [ "$(cat pkg/acme/widget/widget.lk)" = v1 ]; then
  ok "tampered branch download at the locked commit refused"
else bad "tampered branch: rc=$rc out=[$out]"; fi
cp "$TMP/main.good" "$AR/main.zip"

# ── 4) özetsiz eski satır → kurulur ve özet eklenir ───────────────────────────
mkzip "$AR/$A.zip" "$A" deflate "widget.lk=v1" "lib/util.lk=util-v1"
for args in "install" "install --locked"; do
  printf '# look.lock\n"acme/widget" = "main@%s"\n' "$A" > look.lock
  out=$("$LK" $args 2>&1); rc=$?
  if [ $rc = 0 ] && [ "$(echo "$out" | grep -c "recorded checksum $H1 for acme/widget")" = 1 ] \
     && [ "$(lockhash)" = "$H1" ]; then ok "old lock line without a hash works and gets one (lk $args)"
  else bad "old lock line (lk $args): rc=$rc lock=[$(lockline)] out=[$out]"; fi
done

# ── 5) yeni commit → yeni özet ───────────────────────────────────────────────
mkzip "$AR/main.zip" "$B" deflate "widget.lk=v2" "lib/util.lk=util-v2"
echo "$B" > "$AR/main.sha"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
H2=$(lockhash)
if [ $rc = 0 ] && lockline | grep -q "main@$B h1:" && [ -n "$H2" ] && [ "$H2" != "$H1" ] \
   && [ "$(cat pkg/acme/widget/widget.lk)" = v2 ] && echo "$out" | grep -q "moved since look.lock"; then
  ok "moving to a new commit records the new checksum"
else bad "new commit: rc=$rc lock=[$(lockline)] out=[$out]"; fi

# ── 6) bozuk / yarım arşiv → eski paket sağlam ────────────────────────────────
cp look.lock "$TMP/lock.before"
mkzip "$AR/$B.zip" "$B" stored "widget.lk=v3" "zz/second.lk=SECOND-FILE-CONTENT"
corrupt "$AR/$B.zip" "SECOND-FILE-CONTENT"
out=$("$LK" install 2>&1); rc=$?
if [ $rc != 0 ] && echo "$out" | grep -q "could not extract" && [ "$(cat pkg/acme/widget/widget.lk)" = v2 ] \
   && [ -f pkg/acme/widget/lib/util.lk ] && [ ! -e pkg/acme/widget/zz ] \
   && cmp -s look.lock "$TMP/lock.before" && [ "$(leftovers pkg/acme)" = 0 ]; then
  ok "extraction failing midway (CRC error in the 2nd file) leaves the old package intact"
else bad "corrupt archive: rc=$rc out=[$out] files=[$(ls -AR pkg/acme)]"; fi
printf 'this is not a zip' > "$AR/$B.zip"
out=$("$LK" install 2>&1); rc=$?
if [ $rc != 0 ] && [ "$(cat pkg/acme/widget/widget.lk)" = v2 ] && [ "$(leftovers pkg/acme)" = 0 ]; then
  ok "a download that is not a ZIP leaves the old package intact"
else bad "not a zip: rc=$rc out=[$out]"; fi

# ── 7) modüller: atomik değiştirme ────────────────────────────────────────────
MD="$HOME/.look/modules"
out=$("$LK" module install github.com/acme/widget 2>&1); rc=$?
if [ $rc = 0 ] && [ "$(cat "$MD/widget/widget.lk" 2>/dev/null)" = v2 ]; then ok "module install"
else bad "module install: rc=$rc out=[$out]"; fi
cp "$AR/main.zip" "$TMP/main.good"
mkzip "$AR/main.zip" "$B" stored "widget.lk=v3" "zz/second.lk=SECOND-FILE-CONTENT"
corrupt "$AR/main.zip" "SECOND-FILE-CONTENT"
out=$("$LK" module install github.com/acme/widget 2>&1); rc=$?
if [ $rc != 0 ] && [ "$(cat "$MD/widget/widget.lk")" = v2 ] && [ ! -e "$MD/widget/zz" ] \
   && [ "$(leftovers "$MD")" = 0 ]; then ok "failed module extraction leaves the old module intact"
else bad "module corrupt: rc=$rc out=[$out] files=[$(ls -AR "$MD")]"; fi
mkzip "$AR/main.zip" "$B" deflate "widget.lk=v4"
out=$("$LK" module install github.com/acme/widget 2>&1); rc=$?
if [ $rc = 0 ] && [ "$(cat "$MD/widget/widget.lk")" = v4 ] && [ ! -e "$MD/widget/lib" ] \
   && [ "$(leftovers "$MD")" = 0 ]; then ok "module reinstall replaces the old files (no stale files, no temp dirs)"
else bad "module reinstall: rc=$rc out=[$out] files=[$(ls -AR "$MD")]"; fi

echo "installer integrity: $pass passed, $fail failed"
[ $fail = 0 ] && echo "PASS: installer:: look.lock checksum + atomic install" || echo "FAIL: installer:: look.lock checksum + atomic install"
exit $(( fail > 0 ))
