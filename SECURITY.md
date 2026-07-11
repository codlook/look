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

LOOK has **zero third-party dependencies** — every protocol parser (HTTP, RESP2,
MySQL/PostgreSQL wire, SMTP, IMAP, JSON) is our own code, so the entire attack
surface is auditable and hardened in one place. Defense is layered: manual review
+ fuzzing (ASan / UBSan / TSan) + regression tests run on every build + CI.

### Hardened against

| Class | Attack | Defense |
|---|---|---|
| Body DoS | Unbounded request body | `LOOK_MAX_BODY_SIZE` (10 MB default) → 413 |
| Malformed `Content-Length` | Parser exception → worker crash | 400 Bad Request |
| Request smuggling | CL + TE ambiguity | 400 (RFC 7230 §3.3.3) |
| SQL injection | — | Parameterised `?` placeholders + driver-correct escaping |
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
