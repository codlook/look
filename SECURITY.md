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
- **MySQL / MariaDB / Redis default to plaintext.** Use `mysqls://` / `rediss://` (or `?tls=1`) to
  encrypt. Even then, the certificate is **not verified by default** — add `?tls=verify` to
  authenticate the server. Without `verify`, the connection is encrypted but a MITM is still possible.
- The `http::` and PostgreSQL clients verify certificates by default (`SSL_VERIFY_PEER`). The MySQL/Redis clients do not (opt in with `?tls=verify`).

On loopback / same-host / a trusted private network, none of the above is exposed. The runtime logs a
one-time warning per connection pool when a database connection is unencrypted or unverified.

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
