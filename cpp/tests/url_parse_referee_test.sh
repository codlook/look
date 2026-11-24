#!/usr/bin/env bash
# look::parse_url (look/url_parse.h: what http::get and redirect following connect to) against Go's net/url, see
# url_parse_referee_test.cpp: random URLs made by input_fuzz/url_parse_referee.go. LOOK must read the same host, port,
# scheme and user info as Go, and must refuse what Go cannot read.
# Usage: url_parse_referee_test.sh [cases] [seed]   (needs go and g++; 30,000 cases, seed 1 by default)
HERE="$(cd "$(dirname "$0")" && pwd)"
N="${1:-${LOOK_URL_FUZZ_CASES:-30000}}"; SEED="${2:-1}"
command -v go >/dev/null 2>&1 || { echo "  (skipped: go is required for the referee)"; exit 0; }
command -v g++ >/dev/null 2>&1 || { echo "  (skipped: g++ is required)"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
(cd "$HERE/input_fuzz" && go run url_parse_referee.go "$N" "$SEED" > "$TMP/urls.txt") || { echo "FAIL: the referee does not run"; exit 1; }
g++ -O1 -std=c++17 -I"$HERE/../include" "$HERE/url_parse_referee_test.cpp" -o "$TMP/upt" || { echo "FAIL: does not compile"; exit 1; }
"$TMP/upt" < "$TMP/urls.txt"
