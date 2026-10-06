#!/usr/bin/env bash
# Information only: run `lk --check` over the published modules, packages and example
# applications with THIS build, so that every change on this branch shows which of them it
# would stop from loading. It never fails the build; another repository must not be able to
# block this one.
#   does not load   a parse error other than "Undefined function" (that one is expected when
#                   a file of a multi-file application is checked on its own)
#   warnings        capture-stale / arg-count lines from the transition checks
# Usage: ecosystem_scan.sh <lk> [dir-with-checkouts]
LK="$(cd "$(dirname "${1:?usage: $0 <lk> [dir]}")" && pwd)/$(basename "$1")"
DIR="${2:-$(mktemp -d)}"
for repo in look-modules look-packages look-examples; do
    [ -d "$DIR/$repo" ] || git clone -q --depth 1 "https://github.com/codlook/$repo.git" "$DIR/$repo" 2>/dev/null \
        || { echo "== $repo: could not be fetched (skipped)"; continue; }
    files=0; broken=0; warned=0; : > "$DIR/$repo.report"
    while IFS= read -r f; do
        files=$((files + 1)); out="$("$LK" --check "$f" 2>&1)"
        err="$(echo "$out" | grep '^CHECK' | grep -v 'Undefined function')"
        if [ -n "$err" ]; then broken=$((broken + 1)); echo "  does not load  ${f#$DIR/}: ${err#CHECK }" >> "$DIR/$repo.report"; fi
        w="$(echo "$out" | grep -c '^WARN')"
        if [ "$w" -gt 0 ]; then warned=$((warned + 1)); echo "$out" | grep '^WARN' | sed "s|^WARN |  warning        ${f#$DIR/}: |" >> "$DIR/$repo.report"; fi
    done < <(find "$DIR/$repo" -name '*.lk' -not -path '*/.git/*' | sort)
    echo "== $repo: $files files, $broken do not load, $warned with warnings"
    cut -c1-220 "$DIR/$repo.report"
done
exit 0
