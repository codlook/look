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

This is the one change that would otherwise pass silently, so it is reported. A function
that changes a parameter (`$p[i] = …`, `$p.field = …`, `push($p, …)`, `pop($p)`) and then
neither returns it nor passes it on gets one line when the file is loaded — on the command
line, at web startup, and for files loaded with `use`:

```
[WARN ] [check] app.lk:12: function add_tag() changes its parameter $post (push() changes it)
but never returns it or passes it on; arrays and structs are values, so the caller's $post
does not change — return the new value
```

The program still runs. `lk --check` reports the same finding as an error (a `CHECK` line,
exit status 1), so it can stop a build. Run `lk --check` on every file of an application
before moving it to LOOK 2 and return the changed value from each function it names.

The check is deliberately narrow and prefers missing a case to a false alarm: a parameter
the function assigns as a whole (`$p = …`) is its own variable and is not checked, nor is
a variadic parameter.

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

### Reading with `[...]` follows one rule on both engines

```
$row["missing"]                     # null — a key the map does not have
$user = null
$user["name"]                       # null — null reads as an empty map
$row["address"]["city"] ?? ""       # "" when there is no address: the chain is null, `??` does the rest
"abc"[1]                            # error — a string is not an array
$count["x"]                         # error when $count is a number
```

- A key that an array or a map does not have reads as `null`.
- Reading from `null` gives `null`. `null` reads as an empty map, the way reading a nil map
  is not an error in Go; a chain through a missing step is simply `null`, and `??` needs no
  special meaning to give it a default.
- Reading from a string, a number or a boolean is an error. That is not a missing value but
  a wrong type:

  ```
  Index operator requires an array: cannot read ["x"] of an integer — only arrays, maps and
  structs can be indexed (a missing key and null both read as null)
  ```

The same holds for `$x.field`.

What changes from LOOK 1 depends on the engine the code ran on. On the VM (the default)
all of these read as `null`, the wrong types included: `"abc"["k"]` and `$number["k"]` are
errors now. On the tree-walk engine all of them were errors, `null` included: reading from
`null` gives `null` now. Running LookPress and every published module, package and example
on LOOK 2 did not hit the error once.

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

### Route handlers take path parameters by name — silent; an unknown name is an error at load

```
route("GET", "/c/{id}/{slug}", function($slug) { ... })
```

In LOOK 1 path parameters were passed by position: `$slug` above silently received the
`id`, and a parameter the route does not have was silently null. Now each handler parameter
takes the path parameter of the same name, in any order and any subset; the others are read
with `request::param("name")`.

A handler parameter that is not a path parameter of its route stops the application at
startup, with the route and the names it has. In a `WS` or `SSE` route the first parameter
is the connection and can have any name.

Handlers written as in the documentation (`/users/{id}` with `function($id)`) are not
affected. A handler whose parameter names differ from the names in its path must be
renamed.

### Open connections: no total limit, and a default limit per address — silent

LOOK 1 had `LOOK_WS_MAX_CONN` and `LOOK_SSE_MAX_CONN` (1024 each until 1.0.14, off by default
from 1.0.15). LOOK 2 has no limit on the total of open connections and no setting for one: the
operating system's open-file limit is the limit, and the server raises its own soft limit to
the hard limit at start. If one of the two settings is still set, the log says at start that it
is not used.

With no total limit, what keeps one source from taking every file descriptor is a limit per
address. When the server faces the internet directly — `LOOK_TRUSTED_PROXY` is not set — one
address may hold **1000** open WebSocket / SSE connections. The connection over that gets
`429`, and the address is written to the log once until it drops below. Ordinary requests are
not counted. Behind a reverse proxy every connection comes from the proxy's address, so the
default does not apply there: limit per address in the proxy. `LOOK_HTTP_MAX_CONNS_IP=N`, when
set, replaces the default and counts every connection of an address.

What can change for an application: more than 1000 clients behind one address (a large
office, a mobile carrier) that all keep a WebSocket open to a server with nothing in front of
it — the 1001st gets `429`. Set `LOOK_HTTP_MAX_CONNS_IP` to a higher number in that case.

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

### A call to a module function that does not exist stops the application at startup

```
route("GET", "/report", function() { return response::text(string::uper($title)) })
```

```
Error: route GET:/report calls string::uper(), which does not exist — check the spelling of
the module and the function.
```

In LOOK 1 the server started, put that route on the tree-walk engine with a warning, and the
mistake showed on the first request that reached the line. `lk --check` reports the same for
the core modules (`CHECK … [undefined-function]`, exit status 1).

### Job handlers and top-level timers: their own request, on the VM — silent

A job handler (`jobs::worker`) and a timer armed at the top level now run like a request of
their own: on the VM, with the top-level variables at their declared values.

- A handler started from inside a request — a route that calls `jobs::run(0)` — used to share
  that request: `request::get(...)` in the handler read the caller's parameters, and
  `response::status(...)` or `response::header(...)` in the handler changed the caller's
  response. The handler has its own context now and the request keeps its own. Pass what the
  handler needs in the job's payload.
- The callbacks used to run on the tree-walk engine even when the application ran on the VM.
  Nothing changes in what they compute; loop-heavy handlers are much faster (measured:
  4.2 s → 0.08 s for one job).

### A job handler returns `true` or `false`, and a retry waits — was silent

```
jobs::worker("mail", function($job) {
    mail::send(...)          # LOOK 1: no `return` counted as failed and the job was run again
})                           #         at once — three mails. LOOK 2: an error in the log,
                             #         the job is marked failed and is NOT run again.
```

- `return true` — done. `return false`, or an error — failed, tried again later up to the
  job's limit. Anything else is a mistake of the handler: one `ERROR` line names the queue
  and what was returned, and the job is not repeated. Make every handler end with
  `return true` or `return false`.
- A retry waits. In LOOK 1 a failed job was tried again immediately, all tries in the same
  pass. Now the next try comes after 30 seconds, then 60, then 120 (the base is
  `LOOK_JOBS_RETRY_SECONDS`; at most an hour). This also holds for `jobs::fail($id)` called
  by hand: `jobs::next` does not return that job again until its wait is over.

## Statements and the command line

### A line starting with `++` or `--` is a new statement — was an error

```
$x--
--$y        # LOOK 1: Parse Error "Expect ';'"; LOOK 2: decrements $y
```

A line that starts with `+` or `-` continues the expression of the line before; that is
unchanged. A line that starts with `++` or `--` used to be taken the same way and failed to
parse. It is a statement of its own now. Nothing that parsed before changes meaning.

### `++` and `--` work on an element or a field — was an error

`$count["views"]++`, `++$cart.items`, `$grid[$y][$x]--` were refused ("++/-- requires a
variable"). They mean what they mean on a variable: the postfix form yields the old value,
the prefix form the new one. A missing key is an error, as it is for `+=`.

### One engine: the VM — error at load

LOOK 1 had two engines, the VM and a slower tree-walk interpreter, and fell back to the
second when the VM could not run something. LOOK 2 runs only on the VM, on the web server
and on the command line.

- A program the VM cannot compile does not run. The web server does not start (exit
  status 1); `lk` prints the reason and exits with status 1, before any output. An included
  file is named: `Error: use "lib/big.lk": a call has more than 65534 arguments`. In LOOK 1
  the whole program ran on the tree-walk engine instead, about 40 times slower.
- `LOOK_BYTECODE=0` and `LOOK_CLI_VM=0` are ignored, with one `WARN` line. `LOOK_VM_STRICT`
  changes nothing.
- A call to a module function that does not exist is refused before anything runs, on the
  command line as on the web server (`Error: string::uper() does not exist`).
- A script that loads its own files with `use "file.lk"` runs on the VM from the command
  line too (measured on a loop-heavy included function: 3.7 s → 0.07 s).
- A struct declared again with different fields in an included file is an error.

### Code that silently did nothing stops the program — error at load

```
array::push($notes, $note)          # LOOK 2: error — the result is not used
$notes = array::push($notes, $note) # what was meant
```

- The result of a pure built-in is thrown away: `array::push` returns a new array and does
  not change the one you pass, so the line did nothing. The message names the line (and the
  included file). The rule covers built-ins that change nothing and have no other effect —
  `string::`, `math::`, `type::`, `array::` functions without a callback, encoders, hashes,
  date arithmetic, `count()`, `strtoupper()`, `join()` … Built-ins that take a callback
  (`array::map`) or that are called to see whether they throw (`json::decode`,
  `date::parse`) are not in it, and neither is the bare `push()`, which changes its array.
- A route on the path the server answers itself — `GET /health`, or the path in
  `LOOK_HEALTH_PATH` — never ran: the server replied before the application. The application
  does not start; use another path or move the health check with `LOOK_HEALTH_PATH`.

LOOK 1 (1.0.26) reports both with a warning and runs the program.

### Callbacks on the command line: on the VM, output on stdout — silent

A job handler (`jobs::worker` + `jobs::run`), a timer callback and a `parallel()` task run
on the VM from the command line too; in LOOK 1 a script that used `jobs::worker` or `timer::`
ran on the tree-walk engine as a whole. Their `print` output reaches stdout; the tree-walk
engine threw it away (fixed in 1.0.28 as well). `ws::` and `sse::` on the command line are an
error: they belong to a web application.

### A script ends when its last line has run — a warning names the timers left

```
timer::after(400, function() { print("later") })
timer::wait()               # wait for timer::after callbacks (at most 5 s; timer::wait(ms))
```

The program's life is its top level, as in Go and Rust: when the script ends, the process
ends, and a timer that has not fired yet does not run. In LOOK 1 such a timer was lost
without a word. Now one `WARN` line says how many `timer::after` callbacks never ran and how
many `timer::every` timers were still set. To wait, write it: `timer::wait([ms])` waits until
every `timer::after` callback has run and returns `true`, or `false` when `ms` (default 5000)
runs out — the same shape as `parallel::wait`. It does not wait for `timer::every`, which does
not end; stop one with `timer::cancel($id)`. In a web application `timer::wait()` is an error:
a request does not wait for timers.

## Not part of LOOK 2

These arrived in 1.0.x releases and apply to LOOK 1 as well; they are listed because a
program coming from 1.0.0 meets them at the same time.

- Single-quoted strings are raw: `'{$x}'` is the literal text (1.0.1).
- Runtime messages are English, and JSON error bodies use the keys `error` and `code`
  (1.0.6).
- A custom 404 handler keeps status 404 (1.0.7).
