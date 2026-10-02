#!/usr/bin/env bash
# Prints the product version. Single source: project(... VERSION x.y.z) in cpp/CMakeLists.txt.
# Packaging scripts and the release workflow read it from here, so a release needs the
# number changed in exactly one place.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
v="$(sed -n 's/^project(looklang VERSION \([0-9][0-9.]*\).*/\1/p' "$here/../cpp/CMakeLists.txt")"
[ -n "$v" ] || { echo "version.sh: cannot read the version from cpp/CMakeLists.txt" >&2; exit 1; }
printf '%s\n' "$v"
