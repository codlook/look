#!/usr/bin/env bash
# The installer's redirect rule against Go's net/url (see url_redirect_referee_test.cpp): random URLs made by
# input_fuzz/url_referee.go; LOOK must not allow a host Go reads as another, nor a text Go cannot parse.
# Usage: url_redirect_referee_test.sh [cases] [seed]   (needs go and g++; 20,000 cases, seed 1 by default)
HERE="$(cd "$(dirname "$0")" && pwd)"
N="${1:-${LOOK_URL_FUZZ_CASES:-20000}}"; SEED="${2:-1}"
command -v go >/dev/null 2>&1 || { echo "  (skipped: go is required for the referee)"; exit 0; }
command -v g++ >/dev/null 2>&1 || { echo "  (skipped: g++ is required)"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
(cd "$HERE/input_fuzz" && go run url_referee.go "$N" "$SEED" > "$TMP/urls.txt") || { echo "FAIL: the referee does not run"; exit 1; }
g++ -O1 -std=c++17 -I"$HERE/../include" "$HERE/url_redirect_referee_test.cpp" -o "$TMP/urt" || { echo "FAIL: does not compile"; exit 1; }
"$TMP/urt" < "$TMP/urls.txt"
