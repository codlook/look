#!/usr/bin/env bash
# Two examples must never appear in the documentation again, in any file:
#   1. template::render_string("...") with a DOUBLE-quoted template. LOOK substitutes an
#      in-scope variable into the template source before the engine runs: the value skips
#      auto-escaping and is then executed as template code. The template goes in single quotes.
#   2. A SQL statement with a value interpolated into it ({$id}). Values go in as parameters.
# Both were fixed in llms-full.txt in 1.0.1 and stayed on the human-readable page for three
# days, because the same content is kept in two files by hand. Until the two are generated
# from one source, this rule covers every documentation file.
cd "$(dirname "$0")/../.." || exit 1
files="$(git ls-files 'docs/*.html' 'docs/*.txt' 'docs/*.md' 'docs/**/*.html' 'docs/**/*.txt' 'docs/**/*.md' README.md cpp/README.md 2>/dev/null | sort -u)"
fail=0
# In the HTML pages the call is marked up: render_string</span>(<span class="str">"...
bad="$(grep -nE 'render_string(</span>)?\((<span class="str">)?("|&quot;)' $files /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: template::render_string with a double-quoted template:"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
bad="$(grep -nE '\b(WHERE|AND|OR|SET|VALUES)\b[^`"]*\{(<span class="var">)?\$' $files /dev/null || true)"
[ -z "$bad" ] || { echo "FAIL: a value interpolated into SQL:"; echo "$bad" | cut -c1-170 | sed 's/^/    /'; fail=1; }
[ $fail = 0 ] && echo "PASS: no unsafe examples in the documentation ($(echo "$files" | wc -l) files)" || exit 1
