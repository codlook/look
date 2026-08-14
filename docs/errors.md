# LOOK — Error Reference & Troubleshooting

You ran a `.lk` file (or hit a route) and LOOK printed an error. This page explains
what each error **means**, what **causes** it, and how to **fix** it. LOOK is a young
language, so the message alone may not be familiar — find it below.

## How to read a LOOK error

A LOOK error has three useful parts:

```
Runtime Error: Undefined variable: $userr
  at app.lk:42
```

- **The kind** — `Compile Error` / `Parse Error` happen *before* your code runs (syntax);
  `Runtime Error` happens *while* it runs (bad value, missing file, failed connection).
- **The message** — the specific problem. Everything below is organized by this text.
- **The location** (`file:line`) — where it happened. Compile/parse errors point at the
  exact token; runtime errors point at the line being executed.

> Messages are matched by their **leading text**. Where a message ends with `…` or a
> value (`Undefined variable: $x`), look it up by the part before the value.

---

## 1. Syntax & parse errors (before your code runs)

These come from the parser/compiler. Your program never starts until they are fixed.

| Message | Cause | Fix |
|---|---|---|
| `Unexpected token: '…'` | A token appears where the grammar doesn't allow it (a stray `)`, a missing operator, a keyword used as a name). | Look at the token in the message and the one before it. Usually a missing `,` `;` `{` or an extra bracket. |
| `Unexpected character: '…'` | A character that isn't part of LOOK (e.g. a smart-quote `"` pasted from a document, a stray backtick). | Replace it with the ASCII equivalent (`"`, `'`). |
| `Expect ';'. (got '…')` | A statement wasn't terminated. | Add `;` — or check the previous line for an unclosed `(`/`{` that makes the parser think the statement continues. |
| `Unterminated string at line N` / `Unterminated raw string at line N` | A `"…"` (or raw string) opened but never closed on that line. | Close the quote. For text with newlines/quotes inside, use a raw string. |
| `Invalid assignment target.` | The left side of `=` isn't assignable (e.g. `foo() = 1`, `$a + $b = 2`). | Assign to a variable, array element, or property — not to an expression. |
| `an expression is required after throw` | `throw` with nothing after it. | `throw "message"` — throw needs a value. |
| `Expression nested too deep (max N)` / `Expression chain too long (max N)` / `Expression/block nested too deep` | A single expression or block nests far deeper than the limit (often machine-generated code, or a runaway `a.b.c.d…` chain). This limit prevents a parser stack overflow. | Break the expression into intermediate variables / smaller statements. |
| `Jump target too far` | A single function compiled to more than 64 KB of bytecode — a jump can't reach that far. | Split the function into smaller functions. |

---

## 2. Undefined names

LOOK is **strict**: reading a variable that was never assigned is an error, not a silent
`null`. This catches typos early.

| Message | Cause | Fix |
|---|---|---|
| `Undefined variable: $name` | You read `$name` before it was ever assigned — almost always a **typo** (`$userr` vs `$user`) or a variable from another scope. | Assign it first (`$name = …`), or fix the spelling. A function cannot see the caller's variables — pass them as arguments. |
| `Undefined variable read (LOOK_WARN_UNDEF transition mode, returned null): $name` | Same as above, but you set `LOOK_WARN_UNDEF=1`, which downgrades the error to a warning and returns `null`. | Only a migration aid. Assign the variable; unset `LOOK_WARN_UNDEF` to get strict errors back. |
| `Undefined function: name` | You called a function that isn't defined (typo, or you forgot to `use` its module/package). | Check the spelling; add `use <module>` or `use "pkg/<name>"` if it lives in a module/package. |
| `Built-in '<name>' unavailable (not linked)` | A built-in exists in this binary's name table but isn't wired into the running engine (rare; e.g. a CLI-only path hitting a web-only built-in). | Use the documented built-in for your context; report it if a documented function triggers this. |

---

## 3. Types, arithmetic & indexing

| Message | Cause | Fix |
|---|---|---|
| `Arithmetic on a non-numeric value (null/array/object)` | You used `+ - * /` on `null`, an array, or an object. | Convert first (`int()`, `float()`), or check the value isn't `null` before the math. |
| `Arithmetic on an empty string` | Math on `""`. | Guard empty input, or default it (`$n = $s == "" ? 0 : int($s)`). |
| `Arithmetic on a string that cannot be converted to a number: '…'` | The string isn't numeric (`"12abc"`). LOOK does **not** silently truncate. | Validate with `validator::integer/numeric`, or clean the string before converting. |
| `Arithmetic on a numeric string exceeding the int64 limit (bignum/ID): '…'` | A numeric string is larger than a 64-bit integer (often a giant ID). | Keep it as a **string** — don't do arithmetic on it. LOOK compares such IDs exactly as strings. |
| `Division by zero` / `Modulo by zero` | The right operand of `/` or `%` was `0`. | Check the divisor before dividing. |
| `Index operator requires an array` | You used `x[i]` on something that isn't an array/object. | Make sure `x` is an array; check for `null` first. |
| `Array index out of bounds` / `Array index …` | `arr[i]` with `i` past the end (or negative). | Check `count(arr)` before indexing; array indexes are 0-based. |
| `foreach requires an array` | `foreach`/`for … as` over a non-array (often `null` from a lookup that missed). | Ensure the value is an array; default to `[]` when a lookup can miss. |
| `++/-- requires a variable` | `++`/`--` applied to something that isn't a variable (e.g. `++5`, `++foo()`). | Apply it to a variable: `$i++`. |

---

## 4. Function arguments (built-ins)

Built-ins validate their arguments and throw a message of the shape
`name() requires …`, `name() — expects …`, or `… must be a function`. The message names
the exact function and what it wanted. General fixes:

- **`… requires X`** — you passed too few arguments (or the wrong kind). Pass the listed arguments.
- **`… — expects (a, b [, c])`** — brackets mean optional. Provide at least the non-bracketed ones.
- **`… must be a function`** — a callback argument got a value instead of a `function(){…}`.
- **`… must be an array` / `must be a channel` / `must be a websocket`** — the argument is the wrong type; check what you passed.

Representative examples (same pattern applies across `array::`, `string::`, `math::`,
`date::`, `crypto::`, `jobs::`, `queue::`, `ws::`, `sse::`):

| Message | Fix |
|---|---|
| `array::map() requires array and callback` | `array::map($arr, function($x){ … })` — pass both. |
| `array::chunk() size must be positive` | The size argument must be `> 0`. |
| `array::zip() all arguments must be arrays` | Every argument to `zip` must be an array. |
| `math::max() expects 2+ arguments or 1 array` | Call `math::max(a, b, …)` or `math::max($arr)`. |
| `math::sqrt: square root of a negative number is undefined` | Guard against negative input. |
| `date::add() requires (date, amount, unit)` | Pass all three; `unit` is `"day"`, `"hour"`, … |
| `date::parse(): invalid date — '…'` | The string didn't match the given format; check the format argument. |
| `jobs::worker() — second argument must be a function` | Pass a handler `function($job){ … }`. |
| `string::format() requires format string` | The first argument must be the format string. |

---

## 5. Input validation (`validator::`)

`validator::check($data, $rules)` returns field errors; individual rules throw these when a
value fails. `<field>` is your field name.

| Message | Meaning | Fix |
|---|---|---|
| `<field> is required` | Missing or empty value. | Provide the field. (An empty value only fails `required`; other rules pass on empty.) |
| `<field> must be a number` / `must be an integer` | `numeric` / `integer` rule failed — the value wasn't a full number (`"12abc"` is rejected, not truncated). | Send a clean numeric value. |
| `<field> must be at least N` / `must be at most N` | `min:N` / `max:N` failed (length for strings, value for numbers). | Adjust the input to the allowed range. |

See the rule table in the main docs (`required`, `email`, `integer`, `numeric`, `min:N`,
`max:N`, `in:a,b,c`).

---

## 6. Database (`db::`)

**Your API-usage errors:**

| Message | Cause | Fix |
|---|---|---|
| `db::connect() requires DSN string` | Called `connect()` with no DSN. | `db::connect("mysql://user:pass@host/db")`. |
| `db: invalid DSN format` / `db::connect() unsupported DSN scheme: …` | Malformed DSN, or a scheme other than `mysql`/`postgresql`/`sqlite`. | Use a supported scheme and the `scheme://user:pass@host:port/name` shape. |
| `db::query() requires connection and SQL` (and `exec/begin/commit/…` variants) | You didn't pass the connection handle (and SQL). | Pass the handle from `db::connect()` as the first argument. |
| `db: connection not found` / `db: invalid connection handle` | The handle is stale/closed or from a different worker. | Reconnect; don't share a handle across requests — open per request or use the pool. |
| `db: parameter count mismatch — SQL …` | The number of `?` placeholders ≠ the number of bound values. | Match placeholders to the values array exactly. |
| `db: cannot bind NaN/Infinity float as a parameter` | You tried to bind `NaN`/`Infinity`. | Validate/clean the number before binding. |
| `db: query error …` / `db: server error …` | The database rejected the SQL (syntax, constraint, permission). | Read the server's message after the colon; fix the SQL or the data. |

**Connection / network:**

| Message | Cause | Fix |
|---|---|---|
| `db: cannot resolve host: …` | DNS lookup failed. | Check the hostname in the DSN. |
| `db: cannot connect to …` / `db: connection timeout to …` | The server is down, firewalled, or the port is wrong. | Verify host/port and that the DB accepts your IP. |
| `db: connection lost` / `db: connection lost and reconnect failed after …` | The server dropped the connection and auto-reconnect failed. | Check DB stability / timeouts; retry the operation. |
| `db … TLS (…s://) is not supported in this build …` | You asked for TLS (`mysqls://`, `postgresqls://`, `rediss://`) but this binary was built without it. | Use a TLS-enabled build, or connect over a trusted/loopback network. |
| `db postgres: malformed DataRow — …` / `db mysql: column count limit exceeded (malicious server?)` | The server sent a wire response LOOK couldn't trust. | Almost always a broken/incompatible or hostile server; verify you're talking to a real MySQL/Postgres. |

---

## 7. Files & uploads (`file::`)

File access is **sandboxed** to a root directory (`LOOK_FILE_ROOT`); paths can't escape it.

| Message | Cause | Fix |
|---|---|---|
| `file: access denied (path outside LOOK_FILE_ROOT): …` | The path resolved outside the sandbox root (often `../` traversal or an absolute path). | Use a path **inside** the root; set `LOOK_FILE_ROOT` to the directory you intend to serve. |
| `file: invalid path: …` | The path is malformed. | Use a plain relative path like `config/app.json`. |
| `file::read(): cannot open: …` (and `put`/`append`) | The file doesn't exist or isn't readable/writable. | Check the path and permissions; create the file/dir first for writes. |
| `file::read() requires path` (and other `requires`) | Missing argument. | Pass the path (and content for `put`/`append`). |
| `Uploaded file exceeds max_size limit (…)` | An upload is bigger than the allowed size. | Raise the `max_size` option, or reject large files client-side. |
| `File type not allowed: …` | The upload's type isn't in your allow-list. | Add the type to the allow-list, or block it intentionally. |
| `SVG upload requires allow_svg: true option` | SVG is blocked by default (it can carry scripts). | Only set `allow_svg: true` if you sanitize/trust the SVG. |
| `file::store(): upload dir cannot be under web root — set UPLOAD_DIR to a path outside web root` | Saving uploads inside the web root would make them executable/served. | Point `UPLOAD_DIR` outside the public web root. |
| `file::store(): subdir must be a simple name, not a path` | The subdir argument contained path separators. | Use a single folder name (no `/`, no `..`). |

---

## 8. HTTP client (`http::`)

| Message | Cause | Fix |
|---|---|---|
| `http::get() — URL required` (and `post/put/delete/patch`) | No URL (or body) passed. | Pass the URL; `post/put/patch` also need a body. |
| `http:: Unsupported URL scheme: …` | Not `http`/`https`. | Use an `http(s)://` URL. |
| `http:: Invalid IPv6 URL (missing closing ']'): …` | An IPv6 host wasn't bracketed. | Wrap the address: `http://[::1]:8080/`. |
| `http::stream() — the 5th arg (callback) must be a function` | The streaming callback wasn't a function. | Pass `function($chunk){ … }` as the 5th argument. |

---

## 9. Templates (`template::`)

| Message | Cause | Fix |
|---|---|---|
| `Template file not found: …` | The view file path doesn't exist (relative to the views directory). | Check the path/filename passed to `template::render()`. |
| `Template parse error in '…'` | The template syntax is invalid (`{#if}` without `{/if}`, bad tag). | Fix the tag; every `{#…}` block needs its closing tag. |
| `Template security error: escape outside the allowed directory blocked: …` | A `{#extends}`/`{#include}` path pointed outside the views directory. | Reference templates by relative name inside the views root only. |
| `template::render() expects 1 or 2 arguments: (file_path [, $data])` | Wrong argument count/type. | `template::render("page.html", $data)`. |

---

## 10. Crypto (`crypto::`)

| Message | Cause | Fix |
|---|---|---|
| `crypto::sha256() — data required` (and `hmac`, `base64`, `hex`, …) | Missing argument. | Pass the data (and key for `hmac`). |
| `crypto::random_bytes() — must be 1-4096` / `random_string() — must be 1-4096` | Requested length outside 1–4096. | Ask for a length in range; call repeatedly if you need more. |
| `crypto::rs256_sign() — RSA key required (EC/incompatible key rejected)` | The PEM was not an RSA private key. | Provide a PKCS#8 RSA private key. |
| `crypto::rs256_sign() — PEM key parse error` / `RSA key import error (PKCS#8 PEM required)` | The PEM couldn't be parsed. | Check the key is valid PKCS#8 PEM, unencrypted, with proper `-----BEGIN…` lines. |
| `crypto::rs256_verify() — data, sig and PEM public key required` | Missing one of the three arguments. | Pass data, signature, and the PEM **public** key. |

---

## 11. Sessions, cookies & auth

| Message | Cause | Fix |
|---|---|---|
| `session: could not obtain secure randomness (…)` | The OS CSPRNG was unavailable when creating a session ID. | System-level; ensure `/dev/urandom` (or the Windows CSPRNG) is available. |
| `auth::hash() requires password` | No password passed. | Pass the plaintext password to hash. |
| `auth: could not read enough random bytes` | CSPRNG failure while hashing. | System-level randomness problem; check the environment. |

---

## 12. Realtime — WebSocket, SSE, channels

| Message | Cause | Fix |
|---|---|---|
| `ws::send() first argument must be a websocket` (and `on/close`, same for `sse::`) | The first argument wasn't the connection handle. | Pass the connection object you got in the handler. |
| `WebSocket connection limit exceeded (…)` / `SSE connection limit exceeded (…)` | Too many concurrent connections. | Raise the configured limit, or shed/close idle connections. |
| `ws::decode_frame: frame rejected (incomplete or oversized)` | A malformed/oversized WebSocket frame. | Usually a misbehaving client; nothing to fix server-side. |
| `send on closed channel` | You sent to a channel after it was closed. | Don't send after `close()`; coordinate producers/consumers. |
| `channel send/receive timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)` | A channel op blocked past the timeout — often no counterpart is reading/writing (deadlock). | Ensure a receiver exists for every sender; adjust `LOOK_CHANNEL_TIMEOUT_MS` for genuinely slow work. |
| `channel: capacity cannot be negative` | `channel(-1)`. | Use a capacity `>= 0`. |

> Channels/`go{}` are an experimental, opt-in feature — see the docs before relying on them.

---

## 13. Jobs & queues (`jobs::`, `queue::`)

| Message | Cause | Fix |
|---|---|---|
| `jobs::push() — expects (queue, payload [, max_retries [, delay]])` | Wrong arguments. | Pass at least the queue name and payload. |
| `jobs::run() — register a handler first with jobs::worker()` | You ran the worker loop with no handler registered. | Call `jobs::worker("queue", function($job){ … })` first. |
| `jobs:: could not open DB (…)` / `jobs:: schema error: …` | The job store (SQLite) couldn't be opened/initialized. | Check the job DB path and write permissions. |
| `queue::push() — expects (name, value)` (and `pop/peek/size/clear — expects name`) | Missing arguments. | Pass the queue name (and value for `push`). |

---

## 14. Mail (`mail::`)

| Message | Cause | Fix |
|---|---|---|
| `mail:: — MAIL_API_KEY env variable is missing` | The provider API key isn't set. | Set `MAIL_API_KEY` in the environment. |
| `mail:: — MAIL_FROM env variable is missing (or specify the from parameter)` | No sender address. | Set `MAIL_FROM`, or pass `from` to `mail::send()`. |
| `mail:: — MAIL_DOMAIN is required for Mailgun` | Mailgun needs a domain. | Set `MAIL_DOMAIN`. |
| `mail:: — Unknown provider: …` | `MAIL_PROVIDER` isn't a supported provider. | Use a supported provider name. |
| `mail::send() — expects (to, subject [, text [, html [, from]]])` | Wrong arguments. | Pass at least `to` and `subject`. |

---

## 15. Cache & Redis

| Message | Cause | Fix |
|---|---|---|
| `cache::set() — expects (key, value [, ttl])` (and `get/has/delete — expects key`) | Missing arguments. | Pass the key (and value for `set`). |
| `Redis: cannot connect to …` / `Redis: cannot resolve …` | Redis host/port unreachable. | Check the Redis address and that it's running. |
| `Redis AUTH failed` | Wrong Redis password. | Fix the credentials in the connection URL. |
| `Redis TLS (rediss://) is not supported in this build …` | TLS Redis requested on a non-TLS build. | Use a TLS build or a plain `redis://` connection on a trusted network. |
| `Redis: … limit exceeded (…)` | A response exceeded a safety limit (bulk/array/line). | Usually a huge value or a misbehaving server; fetch smaller values. |

---

## 16. Regular expressions (`string::regex*`)

Regex has built-in ReDoS and size protections.

| Message | Cause | Fix |
|---|---|---|
| `string::regex: execution timeout (ReDoS protection — pattern too complex)` | The pattern took too long on this input (catastrophic backtracking). | Simplify the pattern; avoid nested quantifiers like `(a+)+`. |
| `string::regex: concurrent regex limit exceeded (max 8)` | More than 8 regex operations ran at once. | Reduce concurrent regex work; reuse results. |
| `string::regex_match(): input too long (max 65536)` / `pattern too long (max 2048)` | Input/pattern exceeds the size cap. | Trim the input, or pre-filter before matching. |

---

## 17. Modules & packages (`use`, `lk module/install`)

| Message | Cause | Fix |
|---|---|---|
| `Only github.com is supported at the moment.` | An install source other than `github.com`. | Use `github.com/user/repo`. |
| `Invalid package (user/repo required): …` | The path isn't `user/repo`. | Give the full `github.com/user/repo[/subdir]`. |
| `Invalid package <what> (path-escape character): …` | The user/subdir contained `..` or path separators (traversal attempt). | Use plain names — no `..`, no slashes inside a component. |
| `'…' folder not found in the repo.` / `Official module not found.` | The named module/subdir doesn't exist in the repo. | Check the module name and the repo path. |
| `Could not open module file: …` / `Unknown module: '…'` | A `use`d file/module can't be found or loaded. | Check the path in `use`; run `lk module install …` for external modules. |

---

## 18. System / infrastructure errors (rare)

These come from the OS, not your code — you usually can't fix them in `.lk`:
`bind() failed on port …`, `listen() failed`, `socket() failed`, `epoll_create1 failed`,
`eventfd failed`, `CreateIoCompletionPort failed`, `fiber: … failed`,
`… CSPRNG unavailable (/dev/urandom | BCryptGenRandom)`.

Typical causes: the **port is already in use** (`bind() failed`), the process hit a
**file-descriptor / memory limit**, or the OS **randomness source is unavailable** in a
locked-down container. Free the port, raise `ulimit`, or fix the container's `/dev/urandom`.

---

### Still stuck?

- Re-read the message's **leading text** and find its row above — the value after it is your specific input.
- Check the **`file:line`** and the line just before it.
- For behavior questions (what a function expects), see the main API docs.
- If a **documented** function produces `unavailable (not linked)` or a `Compile Error` on
  valid code, that's a bug worth reporting.
