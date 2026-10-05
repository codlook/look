# Moving from LOOK 1 to LOOK 2

LOOK 2 is developed on the `v2` branch and is not released yet. This page lists every
change that can make a LOOK 1 program behave differently, what you see when it does, and
what to write instead. It is updated in the same commit as each change, so it is complete
for the current state of `v2`.

Each entry says how the change shows up:

- **Error at load** — the program does not start; the message names the line.
- **Error at run time** — the statement throws; the message names the cause.
- **Silent** — the program runs and gives a different result. These are the ones to read
  carefully.

## Structs

### One way to write a field — error at load

A field is `name type`, optionally followed by `= default`.

```
struct User {
    name  string
    age   int = 18
    score ?float        # may be null
    tags  array
    boss  User          # another struct; may be null
    note  any           # takes every value
}
```

The two LOOK 1 spellings, a bare `name` and `name: default`, are parse errors. The message
says what to write.

Types: `int`, `float`, `string`, `bool`, `array`, `map`, `fn`, `any`, or the name of a
struct. A leading `?` allows null. An unknown type name stops a web application at
startup, before the first request.

### A field without a default starts at its zero value — silent

In LOOK 1 a field without a default was null. Now `int` starts at `0`, `float` at `0.0`,
`string` at `""`, `bool` at `false`, `array` and `map` empty (a fresh one per instance).
`?type`, `any`, `fn` and struct-typed fields start at null.

Code that tested `$u.name == null` to mean "not set" must declare the field `?string`.

### Values are checked against the field type — error at run time

Both at construction and on every later assignment. An `int` may be written to a `float`
field and is converted. Null is accepted only by `?type`, `any`, `fn` and struct-typed
fields.

### The shape is fixed — error at run time

Reading or writing a field that the struct does not declare is an error. LOOK 1 returned
null on read and silently added the field on write.

### A struct is not an array — partly silent

In LOOK 1 a struct was a map internally, so `array::` functions and `$s["field"]` worked
on it, and `array::set` could bypass the declaration. A struct is now its own kind of
value, and `$s.field` is the only way to read and write a field.

- `$s[0]` is an error at run time.
- `$s["field"]` gives null, `count($s)` gives 0 and `array::` functions treat a struct as
  "not an array" (`array::keys($s)` is empty). These do not raise an error today; code that
  read a struct this way must be changed to `$s.field`.
- `foreach`, `json::encode` and templates still see the fields, in declaration order.

## Arrays, maps and structs are values

### Assigning or passing gives the receiver its own value — silent

```
$a = [1, 2, 3]
$b = $a
$b[0] = 9          # $a is still [1, 2, 3]
```

In LOOK 1 `$a` and `$b` were the same array. The same holds for function arguments and
return values, for maps and for structs, at every depth.

The pattern that breaks is a function that changes its parameter and does not return it:

```
function add_total($order) { $order["total"] = 42 }
add_total($o)      # LOOK 1: $o has "total".  LOOK 2: $o is unchanged.
```

Return the value and assign it:

```
function add_total($order) { $order["total"] = 42
    return $order }
$o = add_total($o)
```

No copy is made on assignment; a value is copied only when it is written to while still
shared, so passing a large array to a function that only reads it costs nothing.

A check that reports functions writing to a parameter without returning it is planned
(a warning at load, an error under `lk --check`). It is not in `v2` yet.

### `push` and `pop` change the variable you name — silent

`push($list, $x)` and `pop($list)` still modify `$list` in place, including paths such as
`push($order["lines"], $x)`. Two differences:

- they change only that variable, not other variables that held the same array;
- `push` returns the new length. In LOOK 1 it returned the array.

### Closures: indexed writes reach the captured variable — silent, tree-walk engine only

Inside `function() use ($list) { ... }`, `$list[0] = 1` and `push($list, 1)` change the
captured variable, and the next call of the closure sees the change. The default engine
already behaved this way in LOOK 1; the tree-walk engine (`LOOK_BYTECODE=0`,
`LOOK_CLI_VM=0`) now follows the same rule.

A closure handed to another thread (`parallel`, timers, WebSocket and SSE handlers) still
gets its own copy of everything it captured.

### Not changed yet

- `==` on two arrays, maps or structs does not compare their content yet. Comparing by
  content is planned for LOOK 2.
- Plain reads and assignments of a captured variable are not settled. Today the two
  engines differ: after `$c = 1; $f = function() use ($c) { return $c }; $c = 2`, `$f()`
  gives 1 on the default engine at the top level of a script and 2 on the tree-walk
  engine, and `$c = ...` inside the closure is rejected by the default engine. LOOK 2
  will have one rule; until then do not rely on either result.

## Web

### The default 404 body is plain text — silent

When no route matches and the application has no handler of its own, the response is
`404 Not Found` as `text/plain`. LOOK 1 answered with a JSON object. A client that parsed
that body must look at the status code, or the application must register its own 404
handler.

## Not part of LOOK 2

These arrived in 1.0.x releases and apply to LOOK 1 as well; they are listed because a
program coming from 1.0.0 meets them at the same time.

- Single-quoted strings are raw: `'{$x}'` is the literal text (1.0.1).
- Runtime messages are English, and JSON error bodies use the keys `error` and `code`
  (1.0.6).
- A custom 404 handler keeps status 404 (1.0.7).
