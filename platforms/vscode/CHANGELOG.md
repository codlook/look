# Changelog

## 2.0.2

- **Added `route::group()`** — Go/chi-style route grouping: shared prefix + middleware inheritance, nestable. Included in IntelliSense with its signature (`route::group($prefix, [$middleware], $fn)`). No grammar change was needed (the general `route::` highlighting already covers it).
- Catalog: 248 → 254 functions (`route::group` + new string-literal built-ins from the source).
- Internal improvement: `gen-builtins.js` SUPPLEMENT merge — when automatic extraction from the docs yields an empty signature (""), the hand-verified signature is used instead (the closure-bodied example of route::group triggered this).
- **Added `string::decode($bytes, $charset)`** — converts a single-byte charset (iso-8859-9, windows-1254, iso-8859-1, windows-1252) to UTF-8; included in IntelliSense with its signature. `http::` applies it automatically when a response declares a charset. `http::` $opts also accepts `follow`/`max_redirects`/`proxy` (the function signature did not change → the catalog is unaffected).

## 2.0.1

- Syntax updated: **semicolons removed** (LOOK has no `;`); `for` clauses are separated by spaces (`for ($i = 0 $i < 10 $i++)`), `switch` is Go-style (no `break`). Added `++`/`--` to the grammar.
- Catalog completed: added the `ws::`, `sse::`, `timer::`, `jobs::run/worker` functions (verified against the source) — 235 → 248 functions; also added to the module highlighting in the grammar.
- Bug fixes: nonexistent `request::query` → `request::get`; WS snippet `$conn->on` → the real `ws::on`/`ws::send`; SSE snippet `sse::send`. Cleaned up leftover HTML in the signatures taken from the docs.
- New brand logo (look-icon).

## 2.0.0

- **Live error checking** rebuilt on top of `lk --check`: parse errors + calls to undefined functions, while typing (onType, debounced) or on save.
- **IntelliSense** generated from the source: 31 modules / 235 built-in functions, 211 with real signatures (hover + completion + signature help).
- Context-aware completion after typing `module::`; `$variable` and `function` symbols from the current file.
- Syntax grammar updated: string interpolation `{$x}`, backtick raw strings, `#` comments, `fn` lambdas, `iota`/`const`/`struct`.
- Commands: Run (`Ctrl+F5`), Start Web Server, REPL.
- Settings: `look.binaryPath`, `look.fcgiPath`, `look.serve.port`, `look.diagnostics.*`.
- New icon (LOOK eye motif).
