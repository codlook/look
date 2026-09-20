#!/bin/bash -eu
# OSS-Fuzz build script for LOOK.
#
# The fuzz targets under cpp/tests/fuzz/ are single translation units: each
# includes only a pure parser-seam header from cpp/include/look/ and defines
# LLVMFuzzerTestOneInput. No linking of the full LOOK binary is required, so the
# build is just "$CXX $CXXFLAGS <target>.cpp $LIB_FUZZING_ENGINE". This mirrors the
# in-repo CI fuzz job (.github/workflows/sanitizers.yml), which compiles the same
# files with clang -fsanitize=fuzzer,address,undefined.

FUZZ_DIR="$SRC/look/cpp/tests/fuzz"
INC="$SRC/look/cpp/include"

# target -> seed-corpus directory (only zipped if the directory exists)
declare -A CORPUS=(
  [fuzz_http_request]=corpus_http
  [fuzz_json_str]=corpus_json
  [fuzz_mysql_lenenc]=corpus_mysql
  [fuzz_pg_parse_error]=corpus_pgerr
  [fuzz_smtp_addr]=corpus_smtp
  [fuzz_imap_parse]=corpus_imap
)

for target in "${!CORPUS[@]}"; do
  $CXX $CXXFLAGS -std=c++17 -I"$INC" \
    "$FUZZ_DIR/$target.cpp" \
    $LIB_FUZZING_ENGINE \
    -o "$OUT/$target"

  seed="$FUZZ_DIR/${CORPUS[$target]}"
  if [ -d "$seed" ] && [ -n "$(ls -A "$seed" 2>/dev/null)" ]; then
    zip -j "$OUT/${target}_seed_corpus.zip" "$seed"/* >/dev/null
  fi
done
