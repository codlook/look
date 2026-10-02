#!/usr/bin/env bash
# The product version has one source: project(... VERSION x.y.z) in cpp/CMakeLists.txt.
# Everything else reads it (platforms/version.sh). This check fails when a second copy
# drifts: the RPM spec, or a documented download name that pins a version number. Docs
# link to the unversioned names (look-lang-linux.zip) that every release also publishes.
cd "$(dirname "$0")/../.." || exit 1
V="$(bash platforms/version.sh)" || exit 1
fail=0
spec="$(sed -n 's/^Version:[ \t]*//p' platforms/linux/rpm/look-lang.spec)"
[ "$spec" = "$V" ] || { echo "FAIL: look-lang.spec Version is '$spec', source says '$V'"; fail=1; }
pinned="$(git grep -nE 'look-lang-(linux|plesk|windows)-[0-9]+\.[0-9]+\.[0-9]+\.zip|codlook/look:[0-9]+\.[0-9]+\.[0-9]+' -- \
    README.md docs platforms .github/workflows 2>/dev/null \
    | grep -v 'release.yml:.*EZİLMEZ' || true)"
[ -z "$pinned" ] || { echo "FAIL: a version number is pinned outside the single source:"; echo "$pinned" | sed 's/^/    /'; fail=1; }
[ $fail = 0 ] && echo "PASS: version $V has a single source" || exit 1
