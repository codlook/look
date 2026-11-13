#!/usr/bin/env bash
# Puts the latest release of the maintenance line (branch release-1.0, tags v1.0.*) into the Plesk
# extension as platforms/plesk/bin-1.0/: a site whose code does not pass the newest release moves
# to the newest release it does pass, so it still gets that line's fixes (sbin/look-setup,
# htdocs/scripts/enable.sh).
#
# Nothing is skipped in silence: when the maintenance line exists, every step must succeed or the
# release stops — a release without it would leave such sites on the release they ran, with no
# fixes, and nobody would notice. The downloaded zip is checked against that release's
# SHA256SUMS.txt, and the binary's build stamp against the tag's commit: a file changed on the
# release page must never travel on to every site.
# Usage (repository root, in the release workflow): bash platforms/plesk/bundle-maintenance.sh
#   needs: git with the origin remote, gh (GH_TOKEN), unzip, sha256sum; GITHUB_REF_NAME, GITHUB_REPOSITORY
set -euo pipefail
die() { echo "::error::maintenance release: $*" >&2; [ -n "${OUT:-}" ] && rm -rf "$OUT"; exit 1; }
REF="${GITHUB_REF_NAME:?}"; REPO="${GITHUB_REPOSITORY:?}"
OUT="platforms/plesk/bin-1.0"

rc=0; git ls-remote --exit-code --heads origin release-1.0 >/dev/null 2>&1 || rc=$?
if [ "$rc" = 2 ]; then echo "maintenance release: there is no release-1.0 branch — nothing to bundle"; exit 0; fi
[ "$rc" = 0 ] || die "could not read the branches of origin (git ls-remote exit $rc)"

# actions/checkout is shallow by default: the ancestry questions below need the history
if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
    git fetch -q --unshallow --tags origin || die "could not fetch the history of the repository"
fi
git fetch -q --tags origin release-1.0 || die "could not fetch release-1.0"
MAINT_HEAD="$(git rev-parse FETCH_HEAD)"
if git merge-base --is-ancestor HEAD "$MAINT_HEAD"; then
    echo "maintenance release: $REF is itself on the maintenance line — nothing to bundle"; exit 0
fi
MT="$(git tag --merged "$MAINT_HEAD" --sort=-v:refname 'v1.0.*' | head -1)"
[ -n "$MT" ] || die "release-1.0 has no v1.0.* tag"
V="${MT#v}"
WANT="$(git rev-parse --short=7 "$MT^{commit}")"

T="$(mktemp -d)"
gh release download "$MT" --repo "$REPO" -p "look-lang-linux-$V.zip" -p SHA256SUMS.txt -D "$T" \
    || die "could not download look-lang-linux-$V.zip and SHA256SUMS.txt of $MT"
line="$(grep -E "^[0-9a-f]{64}  look-lang-linux-$V\.zip\$" "$T/SHA256SUMS.txt" || true)"
[ -n "$line" ] || die "SHA256SUMS.txt of $MT does not list look-lang-linux-$V.zip"
(cd "$T" && printf '%s\n' "$line" | sha256sum -c --status -) \
    || die "look-lang-linux-$V.zip does not match the SHA256SUMS.txt of $MT"
unzip -q "$T/look-lang-linux-$V.zip" -d "$T/x" || die "could not unpack look-lang-linux-$V.zip"
rm -rf "$OUT"; mkdir -p "$OUT"
for b in lk lk-fcgi lk-cgi; do
    f="$(find "$T/x" -name "$b" -type f | head -1)"
    [ -n "$f" ] || die "look-lang-linux-$V.zip has no $b"
    install -m 755 "$f" "$OUT/$b"
    got="$("$OUT/$b" --version 2>/dev/null | head -1)"
    case "$got" in
        "LOOK $V ($WANT,"*) ;;
        *) die "$b of $MT says [$got]; it must be LOOK $V built from $WANT (the tag's commit)" ;;
    esac
done
rm -rf "$T"
echo "maintenance release bundled: $MT ($("$OUT/lk" --version))"
