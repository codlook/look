# Security Policy

## Reporting a vulnerability

If you find a security issue in the LOOK runtime, please report it privately to
**security@codlook.com** (or open a private security advisory on GitHub). Include
a description, the affected version, and a reproduction if possible. Please do not
open a public issue for undisclosed vulnerabilities. We aim to acknowledge reports
within 72 hours.

Supported version: the latest release on the `main` branch.

Machine-readable contact: [`/.well-known/security.txt`](https://look.codlook.com/.well-known/security.txt) (RFC 9116).

---

## Security advisories

### 2026-10-17 — A callback could run in another request after a suspension (1.0.24)

**Affected:** 1.0.20 to 1.0.23 on Linux (fibers, the default dispatch since 1.0.20). **Fixed in:**
1.0.24.

With fibers one thread serves many requests and switches to another while one waits — on a
database query or an outgoing HTTP call. Built-ins that call back into the program (for example
`array::map`, `array::filter`, `array::reduce`) found "the running program" in a variable shared
by the whole thread. When a request waited and another request on the same thread started in the
meantime, a callback of the first request ran in the second request's context: it read the second
request's headers, parameters and session, and its output went to the second request. The process
then often crashed. Measured on the published 1.0.23: 64 clients, each request reading its own
header in an `array::map` callback after an outgoing HTTP call — another user's header came back,
then the process crashed. With `LOOK_FIBER_DISPATCH=0` every answer was correct.

The running program is now kept per request. The same test gives 1920 correct answers of 1920 and
no crash. Routes that the VM cannot compile and that run on the interpreter now also get their own
copy of the interpreter per request in fiber mode (they shared one per thread).

**What to do:** if you run 1.0.20, 1.0.21, 1.0.22 or 1.0.23 on Linux, upgrade to 1.0.24 now —
an application that calls a database or `http::get` and then a built-in with a callback could
show one user's data to another. If you cannot upgrade at once, set `LOOK_FIBER_DISPATCH=0`
until you can.

### 2026-10-17 — A waiting database query held other requests (1.0.22)

**Affected:** 1.0.20 and 1.0.21 on Linux (fibers, the default dispatch since 1.0.20) for
applications that use SQLite, PostgreSQL or MySQL over TLS; and every release up to and
including 1.0.21, in both dispatch models, for applications that use a database at all.
**Fixed in:** 1.0.22.

Three problems, measured on the published 1.0.21:

- **A slow query held its whole thread.** With fibers one thread serves many requests and
  switches to another while one waits. MySQL queries switched; SQLite, PostgreSQL and MySQL
  over TLS held the thread, so the other requests on that thread waited until the query
  ended. With 4 workers: while one 1-second query ran, about one plain request in 20 waited
  up to 1 second (`pg_sleep(1)`: 0.8 s, MySQL `SLEEP(1)` over TLS: 0.9 s, a SQLite write
  waiting for a lock: 1.4 s). With `LOOK_FIBER_DISPATCH=0` none did.
- **A database server that never answered held the request forever.** In fiber mode the
  wait for a MySQL answer ignored the query timeout (`query_timeout`, 30 s by default): the
  request never ended. With `LOOK_FIBER_DISPATCH=0` it failed after the timeout.
- **Every request took a connection from every database pool.** A request borrowed one
  connection from each pool when it started, whether it used the database or not. When a
  pool's connections were all in use, a route that never touches the database waited for
  one too — 1.4 s in the measurement, in both dispatch models.

Now a query that waits suspends only its own request: PostgreSQL and MySQL (with or without
TLS) wait for the server's answer without holding the thread, and the wait ends with an error
after the query timeout. A SQLite query still runs on the request's thread — fast queries pay
nothing extra — but waiting for another program's lock suspends the request, a long statement
gives the other requests a turn every 2 ms, and a write that has to queue behind the other
writers of the database waits on a thread of its own connection. A request takes a database
connection at its first query and keeps it until it ends, so every query of a transaction uses
the same connection.

The same measurements give no waiting request (at most 0.006 s).

*Correction (1.0.23):* in 1.0.22 a long CPU-bound SQLite statement (for example a large
`WITH RECURSIVE`) did give up its turn every 2 ms, but the scheduler only looked for new
connections when no request was ready, so a plain request could still wait ~0.8 s. Fixed in
1.0.23: the scheduler looks for new connections at least every millisecond. Fast queries did not get
slower; in the load test they got faster — SQLite reads and writes 8–17 % more requests per
second, PostgreSQL `SELECT 1` 60–80 % more (requests no longer wait for connections of pools
they do not use).

**What to do:** upgrade.

### 2026-10-15 — Concurrent SQLite writes failed with "database is locked" (1.0.21)

**Affected:** every release up to and including 1.0.20, for applications that write to a
SQLite database (`sqlite://`) from several requests at once. **Fixed in:** 1.0.21.

SQLite lets one connection write at a time. The others sleep and retry until a timeout
(2 seconds for `db::` connections), and that retry is not fair: under steady concurrent writes
some requests kept losing their turn, waited past the timeout and failed with
`database is locked` — a 500, and the row was not written. Measured on the published 1.0.20
with 20 concurrent writers: 21 failed writes in 40 seconds, writes waiting up to 4 seconds;
in a test that also runs transactions and rollbacks, 10–11 failed requests in every run and
the same number of rows missing. Reads were not affected.

Writers of one database in one process now queue in arrival order and take their turn one
after another; reads do not queue. The same load gives no failed request, and the longest
write waits 0.3 seconds instead of 4. A writer that waits more than 10 seconds for its turn
gets the error, so a transaction that is never committed cannot block the others forever.
For another program writing the same file, SQLite's own wait now lasts 5 seconds (was 2).
The error message carries SQLite's extended result code, for example `(code 5)`.

**What to do:** upgrade. Rows that an application believed it wrote while it showed a 500
were not written.

### 2026-10-14 — A few open connections made every other client wait (1.0.20)

**Affected:** every release up to and including 1.0.19, in `--mode http` with no reverse proxy
in front. **Fixed in:** 1.0.20 on Linux; see below for Windows.

The default dispatch gave each open connection a worker until the connection sent a request
or timed out (30 s). As many open connections as there are workers made every other client
wait: connections that sent nothing, one byte or half a request — and also connections that
sent one request and then stayed open, which is what a browser does. Measured on the
published 1.0.19 with 8 workers: a request from another address got no answer for 12 s in
all five cases, and with 8 idle browser-like connections it was answered after 30.5 s. So
this was not only an attack: a handful of ordinary visitors could stall a site that faced
the internet directly. Behind a reverse proxy (nginx, Apache) it did not show, because the
proxy holds the client connections.

From 1.0.20 fibers are the default dispatch on Linux: a connection that waits holds no
worker. The same five cases are answered at once. Measured before the release: every test
of the repository passes in both modes, the LookPress smoke test gives the same result in
both, and a three-hour load test (about 400 million requests) shows the same memory and
error behaviour in both.

**Windows** has no fiber dispatch and keeps the old model. On Windows — and on Linux with
`LOOK_FIBER_DISPATCH=0` — the server logs a warning at start when no reverse proxy is
configured.

**What to do:** upgrade. To go back to the old model on Linux, set `LOOK_FIBER_DISPATCH=0`
(LOOK 2 will not have that switch). On Windows, put a reverse proxy in front and set
`LOOK_TRUSTED_PROXY`, or set `LOOK_HTTP_MAX_CONNS_IP`.

### 2026-10-13 — SMTP `AUTH` sent the password in clear text from any address (1.0.19)

**Affected:** every release up to and including 1.0.18, for applications that run the
built-in SMTP server with a submission port (`LOOK_SMTP_SUB_PORT`) and no certificate.
**Fixed in:** 1.0.19.

`AUTH PLAIN` and `AUTH LOGIN` only encode the password; they do not encrypt it, and the
SMTP server has no TLS. The submission port offered `AUTH` to every client and accepted the
password from any address, so anyone who could read the traffic between the client and the
server could read the user name and the password. Measured on the published 1.0.18.

`AUTH` is now offered and accepted only for a connection from the same machine
(`127.0.0.0/8`, `::1`; the address is the peer address of the socket, no header is
trusted). From any other address it is not advertised and is refused with `530` before the
password is read. The server logs a warning at start.

**This can stop a setup that worked:** a client on another host — or in another container,
which connects from the container network (for example `172.17.0.2`), not from
`127.0.0.1` — can no longer authenticate. Run the client in the same container as the
server, or put a mail relay with TLS in front.

**What to do:** upgrade. If a password was ever sent to the submission port from another
host, change it.

### 2026-10-13 — `STARTTLS` on the SMTP server never completed a session (1.0.18)

**Affected:** every release up to and including 1.0.17, for applications that run the
built-in SMTP server with a certificate (`LOOK_SMTP_CERT` / `LOOK_SMTP_KEY`).
**Fixed in:** 1.0.18, by no longer offering it.

With a certificate set the server advertised `STARTTLS`. The handshake completed and then
the session got no replies at all: a sender that uses TLS when it is offered — most do —
waited, timed out and could not deliver, and such a sender retries for days before it
reports the mail as undeliverable instead of falling back to plain text. Measured on the
published 1.0.16 and 1.0.17. The same handshake ran on a delivery worker with no time
limit, so as many silent `STARTTLS` clients as there are workers stopped all delivery.

From 1.0.18 the SMTP server does not advertise `STARTTLS` and answers the command with
`502`. Senders deliver in plain text and nothing is lost; no client can hold a worker in a
handshake. On the submission port, with a certificate set, `AUTH` is not advertised and is
refused: passwords are not accepted without encryption. The server logs a warning at start
when a certificate is set. IMAP is not affected: its `STARTTLS` and implicit-TLS port work
and are covered by a test now.

**What to do:** upgrade. If you need encryption in transit for SMTP, or authenticated
submission, put a mail relay with TLS in front of the built-in server.

### 2026-10-12 — The SMTP server lost mail sent with pipelining (1.0.17)

**Affected:** every release up to and including 1.0.16, for applications that run the
built-in SMTP server (`LOOK_SMTP_PORT`). **Fixed in:** 1.0.17.

The server advertises `PIPELINING`, which tells a client it may send commands without
waiting for each reply. It did not handle that: after answering one command it left the
following ones in its buffer until more data arrived, and the message body sent next was
read as commands; commands sent right after the final `.` were added to the message that
had just ended and thrown away. Measured on the published 1.0.16: of five messages, the one
whose `MAIL`, `RCPT` and `DATA` came in one packet and the one sent right after a `.` were
lost, with no error on the server. A sending mail server that pipelines (Postfix does) is
ordinary traffic, not an attack.

Commands are now handled in order, one reply at a time, whatever the packet boundaries are.

**Known issue, not fixed in 1.0.17:** `STARTTLS` does not work. The handshake completes and
then the session gets no replies, so a sender that uses TLS cannot deliver (measured on
1.0.16). Until it is fixed, do not set `LOOK_SMTP_CERT` / `LOOK_SMTP_KEY` on the built-in
SMTP server: without a certificate `STARTTLS` is not offered and senders deliver in plain
text. Put a mail relay with TLS in front if you need encryption in transit.

**What to do:** upgrade.

### 2026-10-12 — A late SMTP reply was written to another connection (1.0.16)

**Affected:** every release up to and including 1.0.15, for applications that run the
built-in SMTP server (`LOOK_SMTP_PORT`). **Fixed in:** 1.0.16.

The server answers a finished message from a worker thread. It wrote that answer to the
descriptor number the sending connection had when the message ended. If the sender had
disconnected while the worker was still busy (the SPF lookup can take about 10 s when DNS
does not answer), that number could already belong to a new connection, and the new client
received `250 2.6.0 Message accepted` right after its banner; its commands and replies were
then off by one. Measured on the published 1.0.15: 3 of 3 runs. The message itself was
delivered in every case, and nothing but that one reply line reached the other connection.

The reply is now dropped when its session has ended.

**What to do:** upgrade.

### 2026-10-10 — One connection over the WebSocket / SSE limit stopped the server (1.0.15)

**Affected:** every release up to and including 1.0.14, in `--mode http`, for applications
that have a `WS` or an `SSE` route. **Fixed in:** 1.0.15.

Open WebSocket connections and open SSE connections each had a built-in limit of 1024. The
connection that went over it raised an error that nothing caught, and the server process
ended. Anyone who could reach the port could stop the application by opening 1025
connections to a WebSocket or SSE route — no login and no valid message needed. Measured on
the published 1.0.13 and 1.0.14, for both.

What changed in 1.0.15:

- **There is no built-in limit any more.** The limit is the operating system's open-file
  limit, and the server raises its own soft limit to the hard limit when it starts. Memory is
  the practical bound: in 1.0.x one open WebSocket connection costs the server about 38 KB.
- **At the open-file limit the server waits.** A new connection stays in the listen queue and
  is taken as soon as a place is free; the open connections keep working and the process
  stays up. (1.0.14 did not stop here, but it retried without pausing and used every core.)
- **`LOOK_HTTP_MAX_CONNS_IP` now counts open WebSocket and SSE connections.** Before, a
  connection stopped counting at the upgrade, so one address could hold any number of them.
- **Sockets numbered above 1023 no longer corrupt memory.** The MySQL and PostgreSQL connect
  timeouts and the IMAP idle wait used `select()`, which writes outside its buffer for a
  descriptor above 1023. A server that had more than about a thousand descriptors open (its
  open-file limit raised by the operator, and many connections held) hit this on the next
  database connection: measured under ASan/UBSan with 2000 open WebSockets, `db::connect`
  wrote out of bounds. They use `poll()` now; the same measurement — MySQL, PostgreSQL,
  `http::get` and `mail::send` with 2000 connections open — reports nothing.
- **Request-time built-ins no longer keep the address of an earlier request.** The per-thread
  table of built-ins (`response::`, `request::`, `session::` …) stayed bound to the request
  context it was first built with. On one thread that context usually sits at the same
  address, so the calls landed in the current request; when the address differed they wrote
  to memory of a request that had ended. Found by running the web server under ASan with
  stack-use-after-return detection (`response::text`); no case was observed in a normal build.
  The table is rebuilt when the address changes.
- `LOOK_WS_MAX_CONN` / `LOOK_SSE_MAX_CONN` still work when you set them: the connection over
  the limit is answered `503 Service Unavailable`.

**What to do:** upgrade. With no built-in limit, one address can open connections until the
process has no file descriptors left, and then new requests wait. If the server faces the
internet directly, set `LOOK_HTTP_MAX_CONNS_IP`; behind a reverse proxy, limit connections
per address in the proxy. Check the open-file limit of the service (`LimitNOFILE=` in a
systemd unit) if you expect many clients at once.

### 2026-10-09 — `math::random` repeated and could be predicted (1.0.14)

**Affected:** every release up to and including 1.0.13, on both engines. **Fixed in:** 1.0.14.

`math::random` used the C generator and seeded it again with the current second every time
a copy of the standard library was built — for each worker thread, and for job handlers and
timer callbacks. Two consequences:

- threads created in the same second produced the same sequence. Measured on 1.0.13: 16
  requests at once returned 14 different numbers, and two servers started in the same second
  returned the same first number;
- the sequence followed from the second the process (or thread) started, so whoever knew
  that second could compute the numbers.

An application that made a verification code, a reset code or any other secret with
`math::random(100000, 999999)` gave out codes that could repeat between users and could be
predicted. Session ids, `string::random`, `crypto::random_string`, `crypto::random_bytes` and
`crypto::uuid` were never affected: they read the operating system's secure source.

Every worker thread now has its own generator, seeded from the operating system's secure
source.

**What to do:** upgrade. `math::random` is still not a generator for secrets: if your
application makes codes, keys or passwords with it, change those to `crypto::random_string`
or `string::random`, and consider codes issued before the upgrade as guessable.

### 2026-10-09 — A job handler shared the request that started it (1.0.13)

**Affected:** every release up to and including 1.0.12, on both engines, for applications in
which a route calls `jobs::run(...)`. **Fixed in:** 1.0.13.

A route that calls `jobs::run(0)` runs the queue's handlers before it answers. The handler
ran with that request's context: `request::get(...)` in the handler read the request's
parameters (measured on 1.0.12), and because the whole context was shared its headers and
session were reachable the same way. `response::status(...)` and `response::header(...)` in
the handler changed the request's response — measured: a route answered 503 because the
handler it ran set it.

The handler has a context of its own now.

**What to do:** upgrade. A handler that read `request::` or `session::` values was reading
them from whichever request drained the queue; pass what the handler needs in the job's
payload. Handlers run only from a timer or a separate worker process were not affected.

### 2026-10-07 — Top-level code ran twice at startup; values read at setup were lost (1.0.9)

**Affected:** every release up to and including 1.0.8, on the default engine (the bytecode
VM), in `--mode http` and FastCGI. Applications started with `LOOK_BYTECODE=0` are not
affected. **Fixed in:** 1.0.9.

At startup the server runs the top level of the script twice: once on the tree-walk engine
and once on the VM, which needs it to build the routes. The second pass called some module
functions again and skipped others by a fixed list of name prefixes:

- `file::put` and `file::append` at the top level ran twice: one start wrote two lines to a
  log file (measured on 1.0.8), and a file written at setup was written a second time;
- functions that give a new value on every call (`crypto::uuid`, a timestamp) were called
  again, so the VM pass worked with a second, different value;
- functions on the skip list (`cache::`, `http::`, `mail::`, `queue::`, `jobs::`, and `db::`
  other than `db::connect`) returned null to the second pass. Measured on 1.0.8: a value
  read with `cache::get` at the top level was missing in a route running on the VM, while
  the same route on the tree-walk engine saw it.

Database writes at the top level (`db::exec`) were on the skip list and ran once (measured).

The second pass now replays the results of the first, in order, and calls nothing again.
If the two passes do not make the same calls in the same order, the VM setup stops and the
application runs on the tree-walk engine (`LOOK_VM_STRICT=1` refuses to start); there is no
silently different value.

**What to do:** upgrade to 1.0.9. If your script writes files at the top level, expect one
write per start from now on. If it reads configuration at the top level through `cache::`
or another function listed above, routes will now see the value that was read.

### 2026-10-06 — A failing route handler ran twice in one request (duplicate writes)

**Affected:** every release up to and including 1.0.7, on the default engine (the bytecode
VM), in `--mode http` and FastCGI. Applications started with `LOOK_BYTECODE=0` are not
affected. **Fixed in:** 1.0.8.

When a route handler ended with an uncaught error — a plain `throw`, a failed database
query, an undefined variable, any run-time error — the server treated it as a fault of the
VM and ran the same request again on the tree-walk engine. Everything the handler had done
before the error was done twice in that one request: a row inserted twice, a job pushed
twice, an outgoing HTTP request (a payment, an e-mail, an SMS) sent twice. The client saw
one `500`. The route then stayed on the slower engine until the server was restarted, so a
request that could be made to fail (for example through its input) also slowed that route
down for everyone.

Measured on 1.0.7: a handler that inserts one row and then fails wrote two rows for one
request.

A run-time error is now answered with `500` once; the handler is never run again and the
route stays on the VM.

Related, test mode only: with `LOOK_VM_STRICT=1` the same error escaped the worker without
releasing its database connections, and after as many errors as there are workers the
server stopped answering. Fixed in the same release.

**Behaviour change in 1.0.8.** Routes that call a module function the VM does not provide
(`timer::after`, `timer::every`, `jobs::worker`, ...) used to work only because of that
second run. The server now finds them at startup — through the route, its nested functions
and the global functions it calls by name — runs them on the tree-walk engine from the
first request, and writes one line per route to the log (`Route ... runs on the
interpreter: it reaches ...`). A route that reaches such a function only through a
variable (for example a function stored in an array and called from there) cannot be found
that way; it used to answer after running twice and now answers `500`. Call the function
by name, or start the server with `LOOK_BYTECODE=0`.

Also in 1.0.8, not a security matter: a timer armed while the timer thread was already
waiting (in practice every timer after the first one in the process) could fire up to 30
seconds late. Timers now fire when they are due.

**What to do:** upgrade to 1.0.8. If a handler of yours writes and can then fail, check
your data for duplicates created by failed requests: look for `Dispatch error` or
`VM BUG` lines in the server log and compare with the rows, jobs or outgoing calls made at
those times. Wrapping a multi-step write in `db::transaction` protects the database part
on every version, because the failed attempt is rolled back.

### 2026-10-04 — Timer and WebSocket handlers could still change setup arrays (1.0.5)

**Affected:** every release up to and including 1.0.4, when the application runs on the
interpreter (`LOOK_BYTECODE=0`, or after a VM fallback). **Fixed in:** 1.0.5.

1.0.4 isolated requests from each other, but two kinds of handler slipped through: a timer
armed by a route (`timer::after`, `timer::every`) and a WebSocket message handler
(`ws::on($ws, "message", ...)`). When such a handler called a function that wrote into a
`const` array or a top-level array, it wrote into the real setup value, and every later
HTTP request saw the change. Writes made directly in the handler body, SSE handlers and
the WebSocket open handler were already isolated.

The handlers run on an interpreter copy made from the request's copy; that second-level
copy did not remember where the setup values live. It does now.

**What to do:** upgrade to 1.0.5.

### 2026-10-04 — Arrays created at setup shared between requests (data exposure)

**Affected:** every release up to and including 1.0.3. **Fixed in:** 1.0.4.

The struct issue fixed in 1.0.3 was one case of a wider one. Arrays are shared by
reference in LOOK, and the arrays a web script creates while it is set up were handed to
every request without a copy:

```lk
const { ALLOWED = ["admin"] }
$config = ["features" => []]
route("GET", "/x", fn() use ($config) => ...)
```

A request that wrote into such an array in place — through a `const`, a top-level
variable, a value captured with `use`, or a closure stored in a variable — changed it for
every later request, including other users' requests. With several workers the same array
could also be written concurrently. Both engines behaved the same way.

From 1.0.4 each request works on its own copy: the first time a request touches such an
array it gets a private deep copy, so the next request sees the declared values again.
Requests that do not touch an array pay nothing for it.

**Who is affected:** scripts that modify, in place, an array created outside a route
handler. Reading such arrays was never a problem.

**What to do:** upgrade to 1.0.4. Note that state which used to "survive" between requests
this way no longer does — keep such state in `cache::`, `session::` or the database.

### 2026-10-02 — Struct default arrays shared between requests (data exposure)

**Affected:** every release up to and including 1.0.2. **Fixed in:** 1.0.3.

A struct field whose default is an array or a map was evaluated once, at the declaration,
and every new instance pointed at that same value:

```lk
struct Cart { items: [] }
```

Writing into one instance's `items` in place changed the default itself. In a web
application the declaration outlives the request, so data one request put into its own
`Cart` appeared in the next request's freshly created `Cart` — for a different user. Both
engines behaved the same way. Nested arrays inside the default were shared too.

From 1.0.3 each instance gets its own deep copy of the default.

**Who is affected:** applications that declare a struct with an array or map default and
then modify that array or map in place. Scalar defaults (strings, numbers, booleans) and
fields given a value in the literal (`Cart{items: [...]}`) were not affected.

**What to do:** upgrade to 1.0.3. If you cannot upgrade immediately, give the field a
value in every literal instead of relying on the default.

### 2026-09-30 — Database password written to logs in plain text (action required)

**Affected:** every LOOK 1.0.0 build up to and including build stamp `e7d9636` (all releases
before this fix). **Fixed in:** 1.0.0 builds that include commit `49ecc17` — check the build
stamp with `lk --version`.

When a MySQL/MariaDB, PostgreSQL or Redis connection pool was opened, LOOK wrote an INFO log
line meant to show the DSN without its credentials. The masking took the wrong side of the
`@`, so the line contained exactly the user name and password:

```
[INFO ] [DB] Pool[16] created: mysql://root:S3cretPw9          ← affected builds
[INFO ] [DB] Pool[16] created: mysql://root:***@db:3306/app    ← fixed builds
```

SQLite DSNs carry no password and were not affected. The password was never sent anywhere
else; the exposure is limited to wherever your LOOK logs went (files, journald, a log
collector, support tickets, CI output).

**What to do if you ran an affected build with a password in the DSN:**
1. Upgrade to a fixed build.
2. **Rotate the database password** — upgrading does not remove passwords already written.
3. Purge or restrict access to existing LOOK logs, including copies in log collectors.

---

## Security posture

Every protocol parser (HTTP, RESP2, MySQL/MariaDB & PostgreSQL wire, SMTP, IMAP, JSON) is
our own code kept in one place, so the attack surface is auditable and hardened
in a single spot. Defense is layered: manual review + fuzzing (ASan / UBSan /
TSan) + regression tests run on every build + CI.

### Known limitations

Honesty about what LOOK does **not** protect yet — so you can decide before deploying, not discover after:

- **PostgreSQL TLS verifies by default (secure default).** Use `postgresqls://` (or `?tls=verify`)
  to connect to managed hosts that require TLS — Supabase, Neon, Heroku Postgres, Azure Database
  for PostgreSQL, AWS RDS. The certificate chain **and** hostname are verified (`SSL_VERIFY_PEER` +
  `SSL_set1_host`), so a MITM is rejected. If the server does not offer TLS, the connection is
  **refused with a clear error** — it never silently falls back to plaintext. `?tls=insecure`
  encrypts without verifying (for a self-signed dev cert). Because PostgreSQL TLS is new, there is
  no back-compat pressure to weaken this default — unlike the MySQL/Redis clients below.
- **MySQL / MariaDB / Redis verify by default (secure default).** Use `mysqls://` / `rediss://`
  (or `?tls=1`) to encrypt; the certificate chain **and** hostname are then verified so a MITM is
  rejected. To connect to a server with a self-signed / internal-CA certificate, opt out explicitly
  with `?tls=insecure` (encrypts without verifying) — this is the only switch that disables
  verification, and it emits a one-time runtime warning naming the risk.
- The `http::`, PostgreSQL, **and MySQL/Redis** clients now all verify certificates by default
  (`SSL_VERIFY_PEER` + hostname). The three drivers are aligned.

On loopback / same-host / a trusted private network, none of the above is exposed. The runtime logs a
one-time warning per connection pool when a database connection is unencrypted or explicitly `?tls=insecure`.

**Enforce TLS for remote databases — `LOOK_DB_REQUIRE_TLS` (opt-in).** Set `LOOK_DB_REQUIRE_TLS=1`
to make LOOK **refuse a plaintext connection to a remote host** (MySQL, MariaDB, PostgreSQL, Redis) —
a stricter secure-default than any mainstream Go/Rust driver offers. Loopback and Unix-socket
connections are always allowed (traffic never leaves the host — the common app+DB-same-machine case
is never blocked). To connect a genuinely-trusted remote link in plaintext anyway, either use the TLS
scheme (`mysqls://` / `postgresqls://` / `rediss://`) or add **`?insecure_plaintext=1`** to that DSN as
an explicit, auditable opt-out (named like `?tls=insecure` so "knowingly insecure" is visible in logs).
Loopback detection is **string-based on the literal host** (deterministic, no DNS-resolution/spoofing
surface): a hostname that resolves to `127.0.0.1` is treated as remote — use an IP literal or
`?insecure_plaintext=1` for it. Default off (behaviour unchanged). Especially relevant for Redis, whose
`AUTH` sends the password in cleartext — a plaintext remote Redis exposes the password on the wire.

**Verify-by-default alignment (2026-08 flip).** MySQL/Redis previously defaulted to *encrypt but
don't verify*, out of step with `http::` and PostgreSQL. That inconsistency was silent: a user who
learned "LOOK verifies TLS" was wrong on MySQL and never told so. The default was flipped to
verify-on during the zero-user window (measured: the only known deployment uses plaintext
`mysql://` on localhost — unaffected), with an explicit `?tls=insecure` escape hatch already wired
for self-signed dev/internal certs. The client-side decision (default + escape hatch) is
table-tested in CI (`mysql_redis_dsn_test`, positive-controlled both directions). The real
server-interop matrix (valid-CA / self-signed / hostname-mismatch × MySQL + Redis) is a nightly
gate, not proven by the unit table — same standing as the PostgreSQL TLS interop matrix.

**Cookies are secure-by-default (2026-08 flip).** `cookie::set(name, value)` now emits `HttpOnly`
and `SameSite=Lax` by default — the same alignment reasoning as the DB-TLS flip above: the
`LOOK_SESSION` session cookie already hardcoded those flags, but `cookie::set` set none, so a user
who learned "LOOK cookies are safe" was wrong for application cookies (an auth token stored via
`cookie::set` was JS-readable, stealable through XSS). `HttpOnly` (no JS access) and `SameSite=Lax`
(CSRF mitigation) are now the floor. `Secure` is **not** a default — it would silently break
plain-HTTP dev environments; opt in with `cookie::set(k, v, ["secure" => true])`. Opt out of any
flag with the options map (`["httponly" => false]` when JS must read the cookie, `["samesite" => false]`
to omit it). Positional `expires`/`path` arguments still work unchanged. Verified in CI against a
live `Set-Cookie` header in `--mode http`, FastCGI and CGI (`session_cookie_modes_test`, positive-controlled). This is a
breaking change (a cookie read from JS by name breaks), landed in the zero-user window like the DB-TLS flip.

**The embedded SMTP / IMAP server is off by default and has not had an adversarial security audit.**
The mail servers only listen when you explicitly set `LOOK_SMTP_PORT` / `LOOK_IMAP_PORT` — with no
such env set, no mail thread starts and no port is opened, so a default LOOK deployment exposes no
mail surface. When enabled they ship with real hardening (size caps, `LOGINDISABLED` until TLS,
path-traversal confinement, SMTP relay protection, PBKDF2 + constant-time auth) and pass interop and
fuzz tests. But the ~2,900-line, network-facing, **pre-authentication** parser has not yet been
through a dedicated adversarial audit the way the HTTP / DB / crypto surfaces have. Treat mail as
**experimental**: fine on a trusted or internal network, but have it audited (or keep it behind a
vetted MTA such as Postfix) before exposing SMTP/IMAP to the public internet.

### Hardened against

| Class | Attack | Defense |
|---|---|---|
| Body DoS | Unbounded request body | `LOOK_MAX_BODY_SIZE` (10 MB default) → 413 |
| Malformed `Content-Length` | Parser exception → worker crash | 400 Bad Request |
| Request smuggling | CL + TE ambiguity | 400 (RFC 7230 §3.3.3) |
| SQL injection | `' OR '1'='1`, `UNION SELECT`, `'; DROP TABLE` | Real **native driver binding** (SQLite `sqlite3_bind_*`, MySQL `COM_STMT`, PostgreSQL extended-query) — parameter data never enters the SQL text. `?` is bound **only in the SQL body** — a `?` inside a string literal, quoted identifier or comment is data, and a placeholder/parameter count mismatch is an **error**, never a silent bind |
| Cookie attribute injection | `;` in a cookie value forging attributes (`Domain=`, `Max-Age`, `Path`) to widen scope to sibling subdomains | Name and value percent-encoded on write, decoded on read — a `;` can no longer terminate the value. Distinct from CR/LF splitting below, and it was a **separate channel** |
| Session data injection | `\n` in a stored value forging extra fields (`admin=1`) in the session blob — privilege escalation when user input is stored alongside authorization fields | Key and value escaped on write, unescaped on read: a newline round-trips as data and cannot act as a record separator |
| Client-IP spoofing | Attacker sets `X-Forwarded-For` to choose their own IP, bypassing IP allow/deny lists, bans and per-IP rate limits | The real TCP peer is authoritative. `X-Forwarded-For` / `X-Real-IP` are honoured **only** when the connecting address is in `LOOK_TRUSTED_PROXY`; the chain's first hop is taken. One shared resolver feeds `request::ip()`, the rate limiter, and the WS/SSE paths |
| Slow-header (Slowloris) | Dribbled request headers holding a worker | `LOOK_HEADER_TIMEOUT` (15s total header deadline, timed from the first byte) + `SO_RCVTIMEO/SNDTIMEO` idle timeout + large kernel backlog |
| Slow-body | Full header + large `Content-Length` (or chunked), then a byte every few seconds — holds a pool worker | `LOOK_BODY_TIMEOUT` (30s total body deadline) → 408; opt-in `LOOK_BODY_MIN_RATE` (min average bytes/sec after a 5s grace). Applies to both Content-Length and chunked bodies |
| Per-IP connection exhaustion | One IP opening many concurrent connections to occupy the whole worker pool | Opt-in `LOOK_HTTP_MAX_CONNS_IP` (per-IP concurrent-connection cap → 429). Keyed on the real TCP peer, so enable it only when LOOK is **directly** internet-facing; behind a reverse proxy every connection shares the proxy's IP, so cap at the proxy instead. Complements the per-IP **request-rate** limiter below |
| Path traversal | `../` in file / mailbox / recipient | `weakly_canonical` + root-confinement, `..`/absolute/control-char rejection |
| Integer parsing | Malformed literals in wire protocols | Guarded `stol`/`stoull` (try/catch) everywhere |
| JSON depth | Deeply nested `[[[…]]]` → stack overflow | `JSON_MAX_DEPTH` (256) |
| Parser depth | Deeply nested `((…))` / `[[…]]` in source → stack overflow | `MAX_EXPR_DEPTH` / `MAX_STMT_DEPTH` (150) parser guards |
| Response splitting | CR/LF injected into a response header (`response::header`, `redirect`, cookies) | CR/LF stripped at the setter **and** at header serialization |
| RESP2 OOM | Unbounded bulk / line | `LOOK_REDIS_MAX_BULK` (64 MB), `RESP_MAX_LINE` (64 KB) |
| IMAP OOM | `APPEND {huge}` / infinite line | Size validated before read: `LOOK_IMAP_MAX_LITERAL` (32 MB), `LOOK_IMAP_MAX_LINE` (8 KB) |
| Credential leak | Plaintext IMAP/SMTP login | `LOGINDISABLED` until TLS; auth uses PBKDF2, constant-time compare |
| Recursion | Runtime / VM call depth | `MAX_CALL_DEPTH` guards |
| WebSocket | Oversized frames | RFC 6455 bounds + 16 MB cap |
| WebSocket | Unmasked client frame (RFC 6455 §5.1) | Connection closed with 1002 — no silent accept (cache-poisoning / proxy defense) |

### Additional protections

- **Supply chain:** minimal external runtime dependencies — nothing to install at runtime,
  and the few vendored/linked components (SQLite, miniz, linenoise, OpenSSL) are pinned and
  listed in full in [THIRD_PARTY.md](THIRD_PARTY.md). TLS uses a pinned OpenSSL (statically
  linked in release binaries; dnf-managed in the RPM so `dnf update openssl-libs` applies OS
  security patches without rebuilding LOOK).
- **File sandbox (secure by default):** `file::` access is confined to the current
  working directory when `LOOK_FILE_ROOT` is unset; set it to widen the sandbox, or
  `LOOK_FILE_ROOT=*` to explicitly opt out. Path traversal (`../`) is always blocked.
- **Rate limiting:** two-layer token bucket — global (botnet) + per-IP —
  `LOOK_RATE_LIMIT_RPM` / `_GLOBAL_RPM` / `_BURST`, with `LOOK_TRUSTED_PROXY` for
  real-IP detection behind a reverse proxy.
- **SMTP relay protection:** outbound port 25 blocked; only authenticated 587/465.
- **Memory:** verified leak-free over 1.75M+ requests (RSS steady).

### Verification

Parsers are exercised with ASan/UBSan/TSan fuzzing (16k+ iterations, 0 undefined
behaviour, 0 crashes), end-to-end SMTP→IMAP interop tests, TLS handshake tests
(STARTTLS/IMAPS), and a regression suite gating every build. CI runs the sanitizer
builds on every push.

Every entry in the table above is locked by a regression guard, and the security
ones carry the original attack payload — the cookie check sends
`?v=x; HttpOnly; Domain=evil.com`, the session check sends a newline-forged
`admin=1`, and the IP check sends a spoofed `X-Forwarded-For` **without**
`LOOK_TRUSTED_PROXY` set and requires the real peer back. A guard that cannot fail
on the original bug is not a guard, so each one was verified against the pre-fix
behaviour. The suite runs the same program through all three engines (tree-walk
interpreter, CLI bytecode VM, web VM) and fails on any divergence: a security fix
that only lands in one engine is itself a vulnerability. See
[BUG_AVI_HARITASI.md](BUG_AVI_HARITASI.md) for the method and the full log.

### Resolved issues

- **Session cookies set `Secure` only over HTTPS (fixed a silent-breakage, not a weakening).**
  The `LOOK_SESSION` cookie is marked `Secure` **when the connection is actually HTTPS**, detected
  from `X-Forwarded-Proto: https` — and only when that header comes from a **trusted** proxy
  (`LOOK_TRUSTED_PROXY`), or from an `HTTPS` FCGI param set by the web server; a client cannot forge
  it. Over plain HTTP the flag is omitted, because `Secure` there would only stop the cookie from
  being sent (no protection to add) — which silently broke local `http://` development and any
  reverse-proxy setup that terminates TLS upstream. `HttpOnly` and `SameSite=Lax` are unconditional.
  This is the standard behaviour (PHP/Express/Django); it loses no protection, since `Secure` only
  matters under TLS, which is exactly when it is still set. **Never silent:** if a session cookie is
  shipped without `Secure` over a non-HTTPS request (e.g. behind a TLS proxy with `LOOK_TRUSTED_PROXY`
  unset), the runtime logs a one-time warning so a misconfiguration can't quietly downgrade protection.
  Override with `LOOK_SESSION_SECURE=1` (always) / `=0` (never, for plain-HTTP dev). Guarded by
  `cpp/tests/session_cookie_modes_test.sh` in `--mode http`, FastCGI and CGI (HTTPS, HTTP,
  forged-header, both overrides, and the warning).

- **Cross-request data leak under fiber dispatch (interpreter path) — fixed.** When the
  optional fiber dispatch mode (`LOOK_FIBER_DISPATCH=1`) was enabled *and* a request ran on
  the tree-walk interpreter path (which happens when bytecode compilation falls back for a
  script, not only under an explicit debug flag), the interpreter's `request::`/`response::`
  binding used a dispatch copy shared across the many fibers interleaved on one worker thread.
  When a handler yielded on I/O (`http_client` / `db::query`), another fiber could rebind that
  shared copy to its own request, so the first handler's `request::get()` could then read
  **another user's request data** — one user's response leaking into another's. The default
  worker-pool model (one request per thread at a time) was never affected, and the bytecode VM
  path was already isolated via a per-request builtins snapshot. Fixed by giving every request
  (every fiber) its own dispatch copy in fiber mode. Locked by a concurrent-different-output
  regression guard (`cpp/tests/fiber_ctx/`, wired into CI): 80 concurrent unique-valued requests
  each yielding at an I/O point must every one echo its own value — verified 80/80 leaked before
  the fix, 0 after. Fiber dispatch remains opt-in; the default worker-pool mode was unaffected.

### Platform support tiers

Following the Rust/Go model, platform support is a **declared capability, not a wish** — a tier
says what CI actually proves, so you know what you are getting before you deploy.

| Tier | Platform | Guarantee | Coverage |
|---|---|---|---|
| **Tier 1** | Linux x86_64 | Full suite passes; **blocks the release**. Production target. | CI every push: regression, differential (3 engines), TSan, fuzz (ASan/UBSan), crypto KATs, release gate |
| **Supported** | Docker (Linux image) | Same environment as Tier 1 — the way to run LOOK anywhere, incl. a Windows/macOS box | Image built + smoke-tested at release |
| **Tier 2** | Windows x86_64 (MSVC) | Plain-binary zip ships (`lk.exe` + `lk-fcgi.exe`, **no installer**); core test suite runs in CI, blocks attention (not shipping) | CI (`windows-tier2`) on every `main` push + manual dispatch: MSVC build (`look` + `look-fcgi`) → `crypto_vectors` 17/17 · `rs256_kat` 5/5 · `date_dst` 7/7. Pre-release still adds live `lk-fcgi --mode http` → 200 (gate doesn't exercise it) |

**Windows is a plain binary, not a stack integration.** The earlier mistake was shipping XAMPP
plumbing (an `install.ps1` that patched Apache's `httpd.conf`) — work PHP itself does not do (PHP
ships `php.exe`; XAMPP is third-party). LOOK does not need it: **`lk-fcgi.exe --mode http --port 8080`
is a complete web server** — no Apache, no XAMPP, no config. That is the whole Windows story, and it
is *better* than the single-exe story of Go/Rust (a binary but you write the server) — closer to
`php -S`, with the server built in. So the Windows artifact is just the binaries: download, run, done.

**Out of scope on Windows** — these tools do not exist for MSVC (a technical limit, not neglect):
ThreadSanitizer, libFuzzer, the bash differential/interop suites, **and UBSan** (so the undefined-
arithmetic class, e.g. `-1 * INT64_MIN`, is not caught on Windows — it is caught on Linux). Memory
errors are covered by MSVC AddressSanitizer. This is honest for a dev/small-deploy binary.

**Pre-release Windows checklist (mandatory — the `--mode http` line especially):** the release gate
runs the CLI `lk`, but the whole Windows value proposition is `lk-fcgi --mode http`, which the gate
does not exercise. So each Windows release must also start `lk-fcgi.exe --mode http` and confirm a
live `200` — otherwise a non-booting server could ship (the sibling of the Docker `bare docker run`
bug, which only surfaced when actually run).

**Windows → Tier 1** when the differential/interop suite is portable off bash. The Windows CI matrix
now exists (`windows-tier2`: MSVC build + crypto/rs256/date KATs on every `main` push), so the
MSVC-only branches (NCrypt rs256, BCrypt random, MSVC CRT date) are no longer manual-only. What still
blocks Tier 1 is the bash-bound differential/interop/TSan/fuzz suites (MSVC has no libFuzzer/TSan);
until those are portable, Windows stays Tier 2 — but now with an automated core gate, not a manual one.
