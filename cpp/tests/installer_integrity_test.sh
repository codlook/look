#!/usr/bin/env bash
# installer:: look.lock content digest (h1) + atomic install — NO NETWORK.
#
# Seam: LOOK_INSTALL_ARCHIVE_DIR → the archive is read from <dir>/<user>/<repo>/<ref>.zip instead of GitHub,
# the commit sha from <ref>.sha (imitates GitHub's Content-Disposition). The ZIPs are produced with python3 in the GitHub
# zipball layout ("repo-sha/" prefix).
#
# What is locked:
#  1) the first install writes the digest to look.lock ("ref@sha h1:...")
#  2) same commit + same content (DIFFERENT ZIP bytes) → installs, "checksum verified"
#  3) same commit + changed content → clear error, the old package and look.lock untouched
#  4) an old lock line without a digest → lk install installs and adds the digest; --locked gives an explicit error, does not write to the lock
#  5) switch to a new commit → new digest
#  6) corrupt/partial archive (CRC error, not a ZIP) → the old package is intact, no temp directory is left
#  7) lk module install: the same atomicity (modules have no lock)
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
# corrupt <zip> <marker>: change one byte of the marker in the data (CRC mismatch)
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

# ── 1) the first install records the digest ──────────────────────────────────
mkzip "$AR/main.zip" "$A" deflate "widget.lk=v1" "lib/util.lk=util-v1"
echo "$A" > "$AR/main.sha"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
H1=$(lockhash)
if [ $rc = 0 ] && [ "$(cat pkg/acme/widget/widget.lk 2>/dev/null)" = v1 ] \
   && lockline | grep -q "\"main@$A h1:" && [ -n "$H1" ]; then ok "first install records the checksum ($H1)"
else bad "first install: rc=$rc lock=[$(lockline)] out=[$out]"; fi

# ── 2) same commit, same content, different ZIP bytes → installs ─────────────────
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

# ── 3) same commit, changed content → rejected, nothing changes ──────────
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
# the resolution of lk install github.com/acme/widget comes from main.zip; tamper with that too
cp "$AR/main.zip" "$TMP/main.good"
cp "$AR/$A.zip" "$AR/main.zip"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
if [ $rc != 0 ] && echo "$out" | grep -q "checksum mismatch" && [ "$(cat pkg/acme/widget/widget.lk)" = v1 ]; then
  ok "tampered branch download at the locked commit refused"
else bad "tampered branch: rc=$rc out=[$out]"; fi
cp "$TMP/main.good" "$AR/main.zip"

# ── 4) old line without a digest → installs and the digest is added ───────────────
mkzip "$AR/$A.zip" "$A" deflate "widget.lk=v1" "lib/util.lk=util-v1"
# --locked NEVER writes to the lock, does not install what it cannot verify (Go -mod=readonly): a line without a digest = error
printf '# look.lock\n"acme/widget" = "main@%s"\n' "$A" > look.lock
cp look.lock "$TMP/lock.before"; echo old > pkg/acme/widget/widget.lk
out=$("$LK" install --locked 2>&1); rc=$?
if [ $rc != 0 ] && echo "$out" | grep -q "look.lock has no checksum for acme/widget" \
   && cmp -s look.lock "$TMP/lock.before" && [ "$(cat pkg/acme/widget/widget.lk)" = old ]; then
  ok "lk install --locked refuses a lock line without a hash; look.lock and the package untouched"
else bad "--locked without a hash: rc=$rc lock=[$(lockline)] out=[$out]"; fi
out=$("$LK" install 2>&1); rc=$?
if [ $rc = 0 ] && [ "$(echo "$out" | grep -c "recorded checksum $H1 for acme/widget")" = 1 ] \
   && [ "$(lockhash)" = "$H1" ]; then ok "old lock line without a hash works and gets one (lk install)"
else bad "old lock line (lk install): rc=$rc lock=[$(lockline)] out=[$out]"; fi
out=$("$LK" install --locked 2>&1); rc=$?
if [ $rc = 0 ] && echo "$out" | grep -q "checksum verified"; then ok "after that, lk install --locked verifies it"
else bad "--locked after recording: rc=$rc out=[$out]"; fi

# ── 5) new commit → new digest ───────────────────────────────────────────────
mkzip "$AR/main.zip" "$B" deflate "widget.lk=v2" "lib/util.lk=util-v2"
echo "$B" > "$AR/main.sha"
out=$("$LK" install github.com/acme/widget 2>&1); rc=$?
H2=$(lockhash)
if [ $rc = 0 ] && lockline | grep -q "main@$B h1:" && [ -n "$H2" ] && [ "$H2" != "$H1" ] \
   && [ "$(cat pkg/acme/widget/widget.lk)" = v2 ] && echo "$out" | grep -q "moved since look.lock"; then
  ok "moving to a new commit records the new checksum"
else bad "new commit: rc=$rc lock=[$(lockline)] out=[$out]"; fi

# ── 6) corrupt / partial archive → the old package is intact ────────────────────────
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

# ── 7) modules: atomic replacement ────────────────────────────────────────────
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
