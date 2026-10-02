#!/usr/bin/env bash
# A job may not contain two steps with the same name. Merging branches that each added a
# CI step next to the same line produced the step twice (twice in a row for "Register
# reuse guard"): harmless there, but the same artefact can as easily duplicate a deploy
# or publish step. The same name in two different jobs is fine.
# Usage: ci_no_duplicate_steps.sh [workflow.yml ...]   (default: every workflow)
cd "$(dirname "$0")/../.." || exit 1
[ $# -gt 0 ] || set -- .github/workflows/*.yml
fail=0
for f in "$@"; do
    dups="$(tr -d '\r' < "$f" | awk '
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { job = $1 }
        /^[ \t]+- name:/           { sub(/^[ \t]+- name:[ \t]*/, ""); print job " " $0 }
    ' | sort | uniq -d)"
    if [ -n "$dups" ]; then
        echo "FAIL: duplicated step in $f:"; echo "$dups" | sed 's/^/    /'; fail=1
    fi
    # A step swallowed by a comment: an inserted block that lost its newlines ends up as
    # one "# ... - name: X  run: ..." line. The file still parses and the step never runs
    # (it happened to "Version has a single source", which shipped green without running).
    eaten="$(tr -d '\r' < "$f" | grep -nE '^[ \t]*#.*[ \t]- name:[ \t]' || true)"
    if [ -n "$eaten" ]; then
        echo "FAIL: a step is hidden inside a comment in $f:"; echo "$eaten" | cut -c1-140 | sed 's/^/    /'; fail=1
    fi
done
[ $fail = 0 ] && echo "PASS: no duplicated or hidden CI steps ($# workflow files)" || exit 1
