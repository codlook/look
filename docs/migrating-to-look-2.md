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

### A struct is declared once, at the top level — error at load

A struct declaration must stand at the top level of a file. Inside a function, a closure
or a block it is a parse error.

Declaring the same name a second time with different fields, types or defaults is an
error: a parse error naming the first line when both are in one file, an error when the
second file is loaded otherwise. Reading the identical declaration again is fine.

### A struct is not an array — error at run time

In LOOK 1 a struct was a map internally, so `count()` and `array::` functions worked on
it, and `array::set` could bypass the declaration. A struct is now its own kind of value.

- `$s.field` reads and writes a field. `$s["field"]` does the same and is the way to use a
  field name held in a variable; an unknown name is an error either way.
- `$s[0]`, `count($s)` and every `array::` function given a struct are errors.
- A struct may be an element of a list or a value in a map like any other value.
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

### A closure captures the value — silent; writing to it is an error at load

A closure takes the value of what it captures at the moment it is created, and cannot
change it. This holds for the names in `use (...)`, for the names an anonymous or arrow
function uses from the function around it, and for a named function declared inside
another function.

```
function report() {
    $n = 1
    $f = fn() => $n
    $n = 2
    return $f()        # LOOK 2: 1.  LOOK 1: 2.
}
```

Two things follow.

**A later change outside is not seen (silent).** In LOOK 1 the closure saw the variable as
it was when called. `lk --check file.lk` lists every such place as `WARN ... [capture-stale]`
with the line of the later change. A closure created in a loop keeps the value of its own
iteration.

**A closure cannot change what it captured (error at load).** Assigning to it, assigning to
an element or a field, `+=` and the like, `++`, `push()` and `pop()` on a captured variable
are parse errors; the program does not start. Return the new value and assign it outside:

```
# LOOK 1
$add = function($x) use ($list) { push($list, $x) }
$add(1)

# LOOK 2
$add = fn($list, $x) => array::push($list, $x)
$list = $add($list, 1)
```

A variable a closure assigns before it reads it is the closure's own local, as before, even
when the function around it has a variable of the same name.

Top-level variables are not captured unless they are named in `use (...)`: a function or a
closure that reads `$config` without `use` reads the global as it is when called.

A closure cannot call itself through the variable it is being assigned to
(`$fib = function($n) use ($fib) {...}`): at that moment `$fib` has no value yet. Write a
named function when you need recursion.

A closure handed to another thread (`parallel`, timers, WebSocket and SSE handlers) still
gets its own copy of everything it captured.

### `==` compares content — silent

`==` and `!=` on two arrays, maps or structs compare what they hold, at every depth.
In LOOK 1 the result was always false for arrays and maps.

- lists: same length and equal elements in the same order;
- maps: the same keys with equal values; the order the keys were added in does not matter;
- structs: the same struct name and equal fields;
- an empty list and an empty map are equal.

Elements compare by the usual rule: `[1] == [1.0]` is true, `[1] == ["1"]` is false.
Functions, channels and connections still compare by identity.

## Calls

### A call gives a function the arguments it declares — error at run time

Calling a function with too many arguments, or without an argument for a parameter that
has no default, is an error. The tree-walk engine always did this; the default engine
accepted the call, dropped the extra arguments and made the missing parameter null.

```
function price($amount, $currency) { ... }
price(10)          # LOOK 1, default engine: $currency is null.  LOOK 2: error.
```

A parameter with a default is optional, and a variadic function (`...$rest`) takes any
number above its required ones. The same rule holds when the runtime calls your callback:
`array::map`, `array::filter` and the search functions give one argument, `array::reduce`
and `array::sort` two, `http::stream` two.

`lk --check file.lk` lists the calls it can see (a named function called by name in the same
file) as `WARN ... [arg-count]`.

Route handlers keep their own contract: a handler may declare the path parameters of its
route as its own parameters, in order, or declare none and read them with
`request::param("name")`.

## Scope

### A block scopes its variables at the top level too — error at run time

A variable first assigned inside a block (`if`/`else`, `for` including its init, `while`,
`foreach`, `try`/`catch`, `switch`, a bare `{ }`) is gone when the block ends. Inside a
function this was always so. At the top level of a file the default engine kept the
variable as a global, while the tree-walk engine did not:

```
if ($debug) { $level = "verbose" } else { $level = "quiet" }
print($level)      # LOOK 1, default engine: prints.  LOOK 2: Undefined variable: $level
```

Declare the variable before the block; the block then assigns the one outside:

```
$level = "quiet"
if ($debug) { $level = "verbose" }
```

## Web

### The default 404 body is plain text — silent

When no route matches and the application has no handler of its own, the response is
`404 Not Found` as `text/plain`. LOOK 1 answered with a JSON object. A client that parsed
that body must look at the status code, or the application must register its own 404
handler.

### `http::stream`: the callback takes and returns its state — error at run time

The callback is called as `$callback($chunk, $state)` and what it returns is the state for
the next call. The first state is `$opts["state"]` (null if absent) and the last one is in
the response as `state`. A callback declared with one parameter must add the second.

This replaces keeping the unfinished line in a captured variable, which no longer loads:

```
# LOOK 1
$buf = ["rest" => ""]
http::stream("GET", $url, "", [], function($chunk) use ($buf) { $buf["rest"] = ... })

# LOOK 2
http::stream("GET", $url, "", [], function($chunk, $rest) { ...; return $new_rest },
             ["state" => ""])
```

### Handles are shared, and that is the way to share

Channels, WebSocket and SSE connections and database connections are handles, not values:
copying one gives another reference to the same thing, and a closure that captured a
channel can send on it. They are the deliberate way to share state.

They are not isolated between requests. A channel created at the top level of a web
application and used by a route is the same channel for every request and every worker.
Use that when requests are meant to talk to each other; do not use a handle to get around
the closure rule inside one request — pass the value in and return the new one.

## Not part of LOOK 2

These arrived in 1.0.x releases and apply to LOOK 1 as well; they are listed because a
program coming from 1.0.0 meets them at the same time.

- Single-quoted strings are raw: `'{$x}'` is the literal text (1.0.1).
- Runtime messages are English, and JSON error bodies use the keys `error` and `code`
  (1.0.6).
- A custom 404 handler keeps status 404 (1.0.7).
