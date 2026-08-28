# Security Policy

## Reporting a vulnerability

If you find a security issue in the LOOK runtime, please report it privately to
**security@codlook.com** (or open a private security advisory on GitHub). Include
a description, the affected version, and a reproduction if possible. Please do not
open a public issue for undisclosed vulnerabilities. We aim to acknowledge reports
within 72 hours.

Supported version: the latest release on the `main` branch.

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
live `lk-fcgi --mode http` `Set-Cookie` header (`cookie_flags_test`, positive-controlled). This is a
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
| Slowloris | Slow / idle connections | `SO_RCVTIMEO/SNDTIMEO`, large kernel backlog |
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

- **Supply chain:** zero 3rd-party dependencies; TLS via a pinned OpenSSL (statically
  linked in release binaries; dnf-managed in the RPM so `dnf update openssl-libs`
  applies OS security patches).
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
  `cpp/tests/session_secure.sh` (HTTPS, HTTP, forged-header, both overrides, and the warning).

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
