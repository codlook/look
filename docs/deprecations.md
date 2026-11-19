# Deprecations

Everything that still works today but will stop working in a later release is listed here, in
one place. Each entry says what goes away, what to write instead, whether `lk fix` does it for you,
and how you find it in your code now.

**When:** none of these are removed or turned into errors in any 1.1.x release. They change
together in one later release; its version will be announced in advance, here and in the release
notes. Until then each one only warns.

**Finding them:** run `lk --check` on your code. Every deprecated use in this list except the
JSON keys prints a `WARN … [kind]` line naming the file and line (the kind is given below). At run
time the warnings also go to the log: `push`/`pop` once per file when the application loads, the
missing `use array` once per place when that line first runs.

**Fixing them:** `lk fix app.lk lib/*.lk` rewrites the calls marked "lk fix: yes" in place.
`lk fix --check` lists what it would change and exits 1 if anything would, so it can run in CI.
How `lk fix` rewrites each call, and the cases it leaves to you, is described in the
[upgrade guide](upgrading-from-1.0.md), section "`lk fix` rewrites `push`, `pop` and `array::pop`".

| What goes away | Write instead | `lk fix` | Found by |
| --- | --- | --- | --- |
| `push($list, $x)` | `$list = array::push($list, $x)` | yes | `[push-deprecated]` |
| `pop($list)` | `$last = $list[-1]` and `$list = array::slice($list, 0, -1)` | yes | `[pop-deprecated]` |
| `array::pop($list)` | `$list[-1]` (it never removed anything) | yes | `[array-pop-deprecated]` |
| `array::push` / `array::slice` without `use array` | add `use array` at the top of the file | yes | `[use-array-missing]` |
| the `hata` and `kod` keys in LOOK's JSON error body | read `error` and `code` | no (client side) | — |

## `push()`, `pop()` and `array::pop()`

`push($list, $x)` and `pop($list)` changed the variable they were given, the one exception to the
rule that values are values. `array::pop($list)` returned the last element and removed nothing,
whatever its name says. There is one way to add to and take from a list:

```
$list = array::push($list, $x)       // add at the end (done in place, no copy)
$last = $list[-1]                    // the last element
$list = array::slice($list, 0, -1)   // all but the last (done in place, no copy)
```

**Will become:** an error.

**Check by hand after `lk fix`:**

- On an empty list `pop()` and `array::pop()` returned `null`; `$list[-1]` stops with an error. Test
  `count($list) > 0` first where the list can be empty. `lk fix` prints a `NOTE` with the file and
  line for each such rewrite.
- `push($m["new"], $x)` created a missing key; `$m["new"] = array::push($m["new"], $x)` stops with an
  error when the key is missing. Set it to `[]` first where it can be missing.
- A call inside a larger expression (`print(pop($l))`) is not rewritten; `lk fix` reports it as
  `not rewritten` and exits 1.

Details: [upgrade guide](upgrading-from-1.0.md), sections on `push($list, $x)` and `pop($list)`.

## `array::push` and `array::slice` without `use array`

`$list = array::push($list, $x)` and `$list = array::slice($list, 0, -1)` ran without `use array`,
because this in-place form skipped the module check, while every other `array::` call stops with
`Module 'array' not loaded.`. Today the in-place form still runs and warns.

**Will become:** the same `Module 'array' not loaded.` error as every other `array::` call.
`lk fix` adds `use array` when it rewrites a file.

## `hata` and `kod` in the JSON error body

When `lk-cgi` or `lk-fcgi` cannot run the script at all (no script given, or the application fails
to load), the answer is a JSON body with `error` and `code`. It also carries `hata` and `kod`, copies
of the same values under their old names. The body `--mode http` sends when a request fails
unexpectedly carries `hata` as well.

**Will become:** the body has only `ok`, `error` and `code`. A client that reads `hata` or `kod`
should read `error` and `code` now; they have been sent alongside `hata` and `kod` since 1.0.6.
