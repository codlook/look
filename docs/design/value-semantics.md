# LOOK 2 design: value semantics for arrays, maps and structs

Status: **proposal — not implemented.** Branch: `v2`.

## The problem

In LOOK 1, arrays are shared by reference. `$a = $b` does not copy, passing an array to a
function does not copy, and `$a[0] = x` changes the array for every name that refers to it.
This is undocumented behaviour that PHP-shaped syntax leads people to expect the opposite of,
and it is the single root of a whole class of defects found in 1.0.x:

| Defect | Release that patched it |
|---|---|
| A struct default array shared by all instances — one request's data in the next user's instance | 1.0.3 |
| `const` arrays, top-level arrays and captured arrays writable across requests | 1.0.4 |
| Timer and WebSocket handlers writing into setup values | 1.0.5 |
| `array::slice` copies one level only; no real deep copy exists | documented, not fixed |
| `==` on arrays compares identity, not content | open |

Each patch added a mechanism (per-instance deep copies, lazy per-request copies, environment
rebinding). They close the holes one by one. Value semantics removes the cause.

The language is also inconsistent with itself: `array::push` returns a new array (value
style) while `$a[0] = x` mutates in place (reference style). One job, two models.

## The rule

> **Assigning, passing or returning an array, a map or a struct gives the receiver its own
> value. Changing one never changes another.**

That is the whole rule. There is no reference type and no way to opt out. Sharing mutable
state between requests stays where it already belongs: `cache::`, `session::`, the database.

```lk
$b = [1, 2]
$a = $b
$a[0] = 9          # $b is still [1, 2]

function zero($x) { $x[0] = 0; return $x }
$c = zero($b)      # $b is still [1, 2]; $c is [0, 2]

struct User { name string, tags array }
$u = User{name: "Ada"}
$v = $u
$v.name = "Bo"     # $u.name is still "Ada"
```

A function that wants to change a caller's value returns the new value. This is the same
style `array::push`, `array::map` and every other `array::` function already use.

## How it stays fast: copy on write

Nothing is copied at assignment. The array storage is shared and reference counted, exactly
as today. A copy is made only when someone **writes** to storage that is still shared; storage
with a single owner is written in place.

- Passing a 5 000-element list to a function that only reads it: no copy.
- A loop that fills or updates its own array: no copy, in place, O(1) per write.
- `$b = $a; $b[0] = x`: one copy of `$a`'s top level at the first write, then in place.

Nested data is copied one level at a time, on the path that is written:
`$m["rows"][3] = x` makes `$m` unique, then `$m["rows"]` unique, then writes. Sibling
branches stay shared.

### The part that needs care

Today a write compiles to "load the array into a register, write through it". With
reference semantics that register is just another handle. With copy on write, the register
is a *second owner*, so a naive implementation would copy on every single write to a global
or nested array — quadratic loops. The compiler therefore has to emit writes as a path:
resolve the variable's own slot, make each level unique in that slot, write. Locals already
work this way (the register *is* the variable). Globals, captured variables and nested paths
need the new code path. This is the bulk of the work, and it is why the benchmark below
exists before any code.

The same applies to the tree-walk interpreter, which stays as the test oracle: index
assignment must work on the variable's slot, not on a copy of the value.

## Answers to the design questions

**Function parameters.** By value. The callee's writes are never visible to the caller.

**Closure capture (`use ($x)` and automatic capture).** A closure captures the *variable*,
as today for scalars: the cell model stays. The value inside the cell follows the rule above,
so two closures sharing a cell see each other's writes to that variable (that is one
variable), but copying the array out of it gives an independent value.

**Structs.** Value types, like arrays. This is the one decision with a real cost in
familiarity: PHP objects are handles, LOOK structs would not be. The reasons for it:
one rule instead of two; a struct holding an array would otherwise be half value, half
reference; and a struct handle shared between requests is the 1.0.3 leak again. A struct
also stops being an array internally — it becomes its own value kind with fixed slots, so
`array::` functions reject it and field access is an index, not a string search. That closes
the "`array::set` bypasses the type check" gap and is the speed work that was planned next.

**`array::` functions.** Unchanged: they already return new values. After this change index
assignment follows the same model, so the two styles become one.

**Equality.** `==` on arrays, maps and structs compares content, recursively. Identity
comparison disappears with identity.

**Thread safety.** The reference count is atomic. "Unique" means the count is 1, which can
only be observed by the single owner, so in-place writes need no lock. A value shared with
another thread has a count above 1 and is copied before the write.

**The 1.0.4 / 1.0.5 isolation machinery.** Becomes unnecessary: a request that writes to a
setup array copies it by the general rule. The lazy per-request copy in `LOAD_GLOBAL`,
`VM::request_local` and `Interpreter::request_env` are removed. Their tests
(`request_isolation_test.sh`, `struct_default_isolation_test.sh`,
`realtime_isolation_test.sh`) stay and must pass unchanged — they are the proof that the
general rule covers every case the patches covered.

## What breaks

Code that relies on sharing:

- a function that mutates an array argument and returns nothing — must return the array;
- a struct passed around and mutated in place as an "object" — same;
- two variables deliberately aliased to one array.

Each of these changes silently, not with an error: the code runs and the caller no longer
sees the change. That is the dangerous kind of break, so the migration needs a tool:
`lk --check` should flag a function that writes into a parameter and never returns or
stores it. This tool is part of the work, not an afterthought.

## Baseline (before any change)

`cpp/bench/value/run.sh`, best of 3, milliseconds, container-local files.

| Workload | VM | tree-walk | What it guards |
|---|---|---|---|
| `local_write` — 600 000 in-place writes to a local list | 54 | 1373 | must stay O(1) per write |
| `global_write` — the same on a top-level list | 64 | 1371 | the path that would go quadratic if done naively |
| `nested_write` — 300 000 writes two levels deep | 103 | 918 | path copy must not copy per write |
| `pass_read` — 300 000 calls passing a 5 000-element list | 53 | 2853 | passing must not copy |
| `assign_then_write` — 20 000 × (`$b = $a`, write `$b`) on 2 000 elements | 18 | 129 | the one case that now pays for a real copy |

Acceptance: the first four within 10% of these numbers. `assign_then_write` will get
slower — it performs 20 000 copies of a 2 000-element array that today it does not perform,
because today it is silently changing `$a`. Its output also changes from
`199990000:19999` to `199990000:0`, which is the point.

## Order of work

1. Structs become their own value kind with slots (no behaviour change yet; `array::`
   rejects structs; field access by index).
2. Copy-on-write writes in the VM: locals, globals, captures, nested paths.
3. The same in the tree-walk interpreter.
4. Content equality.
5. Remove the 1.0.4/1.0.5 isolation machinery; the isolation tests must still pass.
6. `lk --check` rule for writes into a parameter.

Each step lands with its own guard, proven by fault injection, and the benchmark above run
before and after.
