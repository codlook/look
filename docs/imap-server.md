# LOOK — Embedded IMAP Server

LOOK includes an embedded **IMAP4rev1** server (RFC 3501 + RFC 2177 IDLE) with minimal
external runtime dependencies. No external service (Dovecot, Courier) is needed. It shares
the same Maildir store with the embedded SMTP server: **SMTP delivers the mail → IMAP serves
it to clients.** It is active only in `--mode http`, and only in Linux/POSIX builds (the
embedded mail servers are not part of the Windows build).

Clients can connect and **list, read, write (APPEND), flag, delete, search, and receive
instant notifications (IDLE push)** — all over TLS. Messages are addressed by sequence
number: persistent UIDs are not supported yet, so the `UID` command and the `UID` FETCH item
are answered with `NO [CANNOT]`. Clients that depend on UID commands will not work fully.

---

## Quick Start

Add to your `.env` file:

```
# --- Incoming mail chain (SMTP receives → Maildir → IMAP serves) ---
LOOK_SMTP_PORT=25                 # MTA (server→server delivery)
LOOK_SMTP_SUB_PORT=587            # Submission (authenticated outgoing)
LOOK_SMTP_LOCAL_DOMAINS=myapp.com # local domains (open relay prevention)

LOOK_IMAP_PORT=143                # IMAP (upgraded with STARTTLS)
LOOK_IMAP_PORT_TLS=993            # IMAPS (implicit TLS) — optional
LOOK_IMAP_CERT=/etc/look/mail.crt # TLS certificate (if unset, LOOK_SMTP_CERT is shared)
LOOK_IMAP_KEY=/etc/look/mail.key

# --- Shared identity + store ---
LOOK_SMTP_USER_DB=mysql://user:pass@127.0.0.1/mydb  # mail_users table (read by SMTP AUTH and IMAP LOGIN)
LOOK_MAIL_DIR=/var/mail/look                          # Maildir root
```

```bash
lk-fcgi --mode http --port 7400 --workers 4
# At the same time: HTTP :7400 · SMTP :25/:587 · IMAP :143 · IMAPS :993
```

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LOOK_IMAP_PORT` | — | IMAP port (143). If empty, IMAP is disabled. An invalid value logs an error and IMAP does not start. |
| `LOOK_IMAP_PORT_TLS` | 0 | IMAPS / implicit TLS port (993). Opened only when a certificate is configured. |
| `LOOK_IMAP_CERT` | — | TLS certificate (PEM chain). If unset, `LOOK_SMTP_CERT` is used. |
| `LOOK_IMAP_KEY` | — | TLS private key (PEM). If unset, `LOOK_SMTP_KEY` is used. |
| `LOOK_MAIL_USER_DB` | — | `mysql://...` DSN. LOGIN is checked against the `mail_users` table. If unset, `LOOK_SMTP_USER_DB` is used. (SMTP AUTH reads only `LOOK_SMTP_USER_DB`, so set that one to share users between SMTP and IMAP.) |
| `LOOK_MAIL_USER` / `LOOK_MAIL_PASS` | — | Single-user fallback when no DB is configured (dev / simple deployment). Constant-time comparison. |
| `LOOK_MAIL_DIR` | `/var/mail/look` | Maildir root. The user's INBOX: `<dir>/<user>/inbox`. |
| `LOOK_IMAP_MAX_CONN` | 1000 | Maximum concurrent connections (DoS limit). |
| `LOOK_IMAP_MAX_LINE` | 8192 | Command line limit (line bomb → connection is closed). |
| `LOOK_IMAP_MAX_LITERAL` | 33554432 | APPEND literal limit (32 MB). If exceeded: `NO [TOOBIG]` **without reading the data**. Messages larger than this are also skipped by FETCH and SEARCH. |
| `LOOK_IMAP_MAX_ERRORS` | 5 | Protocol error limit per connection (then disconnect). |
| `LOOK_IMAP_AUTH_DELAY_MS` | 500 | Fixed delay after a failed LOGIN (slows down brute force). |
| `LOOK_IMAP_IDLE_TIMEOUT` | 1800 | Receive timeout for an idle connection, in seconds (Slowloris + RFC autologout). |
| `LOOK_IMAP_IDLE_TICK_MS` | 15000 | Mailbox scan interval during IDLE (new-mail detection). |

---

## Supported Commands

| Command | Status | Note |
|---------|--------|------|
| `CAPABILITY` | ✅ | `IMAP4rev1 IDLE` (+ `STARTTLS LOGINDISABLED` on plain text when a certificate is configured) |
| `NOOP` · `LOGOUT` | ✅ | |
| `STARTTLS` | ✅ | RFC 3501 §6.2.1 — the session state is reset afterwards |
| `LOGIN` | ✅ | `mail_users` pbkdf2 or the single-user fallback |
| `SELECT` / `EXAMINE` | ✅ | Stable sequence snapshot; EXISTS/RECENT/UIDVALIDITY/FLAGS |
| `LIST` / `LSUB` | ✅ | Maildir++ subfolders (`.Sent` → `Sent`) |
| `STATUS` | ✅ | MESSAGES / RECENT |
| `FETCH` | ✅ | seq-set, `FLAGS` `RFC822.SIZE` `BODY[]` `BODY[HEADER]` `BODY[TEXT]` `RFC822` `RFC822.HEADER` (the `UID` item → `NO [CANNOT]`) |
| `STORE` | ✅ | `+FLAGS` `-FLAGS` `FLAGS` (+`.SILENT`), Maildir `:2,` rename |
| `EXPUNGE` | ✅ | Deletes messages marked `\Deleted` + `* n EXPUNGE` notification |
| `APPEND` | ✅ | literal + flags; size limit checked up front against OOM |
| `SEARCH` | ✅ | see below |
| `IDLE` | ✅ | RFC 2177 — live new-mail push (after `SELECT`) |
| `UID` | ❌ | `NO [CANNOT]` — no persistent UIDs yet; use sequence-based FETCH/STORE/SEARCH |
| `CREATE` · `DELETE` · `RENAME` · `SUBSCRIBE` · `UNSUBSCRIBE` | ❌ | `NO [CANNOT]` — not supported |

### SEARCH criteria

Multiple criteria are **AND**ed. Quoted arguments are supported (`SEARCH FROM "ali veli"`).
Text matching is case-insensitive.

```
ALL · SEEN/UNSEEN · DELETED/UNDELETED · FLAGGED/UNFLAGGED · ANSWERED/UNANSWERED
DRAFT/UNDRAFT · NEW/OLD/RECENT
FROM · TO · CC · SUBJECT   (header field, per field — no false positives)
BODY · TEXT                (body / full text — memory-bounded read)
HEADER <field> <value>
<seq-set>  (e.g. 1:3, 2:*)  ·  UID <seq-set>  (read as a sequence set)
CHARSET <name>             (accepted and ignored)
```

Unknown criterion → `BAD`. Body/full-text search reads at most `LOOK_IMAP_MAX_LITERAL` bytes
per message (no OOM).

---

## IDLE — Instant Notification (RFC 2177)

Professional and mobile clients receive new mail via **push** instead of polling:

```
C: a1 IDLE
S: + idling
   ... (SMTP delivers a new message) ...
S: * 2 EXISTS
S: * 2 RECENT
C: DONE
S: a1 OK IDLE completed
```

During IDLE the server watches both the socket (`DONE`) and the mailbox (every
`LOOK_IMAP_IDLE_TICK_MS`). New messages are appended to the **end** of the current sequence
snapshot — sequence numbers within a session stay fixed, as RFC 3501 requires.

---

## User Database (`mail_users`) — Shared with SMTP

IMAP LOGIN uses **the same table and the same pbkdf2 verification** as SMTP AUTH.
A single user record is valid for both outgoing (SMTP submission) and incoming (IMAP) mail.

```sql
CREATE TABLE mail_users (
    id       INT AUTO_INCREMENT PRIMARY KEY,
    email    VARCHAR(255) UNIQUE NOT NULL,
    password VARCHAR(255) NOT NULL,  -- pbkdf2$sha256$... (generate with auth::hash)
    active   TINYINT DEFAULT 1
);
```

```lk
route("POST", "/admin/mail-user-add", function() {
    $conn = db::connect(env("DB_DSN", ""))
    $hash = auth::hash(request::post("password"))
    db::exec($conn,
        "INSERT INTO mail_users (email, password, active) VALUES (?, ?, 1)",
        [request::post("email"), $hash]
    )
    response::json(["ok" => true])
})
```

After login, the user name (e-mail) becomes a Maildir path component; therefore names that
contain `/`, `\`, `..` or control characters are rejected (defense in depth).

---

## Maildir Alignment — SMTP ↔ IMAP

The embedded SMTP server delivers to a separate Maildir for each **local recipient**; IMAP
`INBOX` reads exactly the same location:

```
/var/mail/look/
  alice@myapp.com/
    inbox/
      new/  1751234567.abc   ← message delivered by SMTP
      cur/                    ← read/flagged (STORE → :2,S rename)
      tmp/
  bob@myapp.com/
    inbox/ ...
```

So a message that arrives with `RCPT TO:<alice@myapp.com>` appears in the INBOX of the client
that does `LOGIN alice@myapp.com`. Additional folders (Sent, Drafts) are stored with the
Maildir++ dot prefix (`.Sent`) and appear as `Sent` in `LIST`. Folders are not created over
IMAP (`CREATE` is not supported); create the directory on disk.

---

## TLS — STARTTLS and IMAPS

Both upgrade paths are supported:

- **STARTTLS (143):** The client connects in plain text and upgrades to TLS with `STARTTLS`.
  When a certificate is configured, the server advertises `LOGINDISABLED` and refuses `LOGIN`
  before TLS with `NO [PRIVACYREQUIRED]` — **credentials never travel unencrypted.**
- **IMAPS (993):** TLS handshake as soon as the connection opens (implicit TLS).

TLS 1.2 is the minimum; SSLv2/SSLv3/TLS1.0/TLS1.1 are disabled. The certificate comes from
`LOOK_IMAP_CERT/KEY`, or, if those are unset, the embedded SMTP server's `LOOK_SMTP_CERT/KEY`
is shared. If there is no certificate at all, the server runs in plain text (development only).

```bash
# Self-signed test certificate
openssl req -x509 -newkey rsa:2048 -keyout mail.key -out mail.crt \
  -days 365 -nodes -subj "/CN=mail.myapp.com"
```

---

## Security Features

| Feature | Detail |
|---------|--------|
| Credential privacy | With TLS configured, plain-text LOGIN is refused (`LOGINDISABLED` / `PRIVACYREQUIRED`) |
| OOM protection | APPEND literal size is checked **before reading** (`LOOK_IMAP_MAX_LITERAL`) → `NO [TOOBIG]` |
| Line bomb | Command line limited by `LOOK_IMAP_MAX_LINE` |
| Connection flood | `LOOK_IMAP_MAX_CONN` concurrent connection ceiling |
| Slowloris | `SO_RCVTIMEO` idle timeout (`LOOK_IMAP_IDLE_TIMEOUT`) |
| Brute force | Fixed delay after a failed LOGIN + error limit (closes the connection) |
| Path traversal | mailbox name + user name + SMTP recipient — `..`/absolute path/control characters rejected at three layers + `weakly_canonical` check that the path stays inside the root |
| Sequence stability | RFC 3501 — sequence numbers are fixed within a session; only EXPUNGE changes them (prevents deleting the wrong message) |
| Constant-time comparison | pbkdf2 verification and the single-user fallback, against timing attacks |

Verification: ASan+UBSan fuzzing (overflowing/negative literals, malformed seq-sets,
tokenizer edge cases, protocol violations, path traversal bombardment) → **0 UB, 0 crashes.**
End-to-end interop test: SMTP delivery → disk → IMAP LOGIN+SELECT+FETCH reads the same message.

---

## Testing

```bash
# STARTTLS + LOGIN (openssl s_client handles STARTTLS automatically)
openssl s_client -quiet -crlf -starttls imap -connect localhost:143
a1 LOGIN alice@myapp.com password
a2 SELECT INBOX
a3 FETCH 1 (BODY[HEADER])
a4 LOGOUT

# IMAPS (implicit TLS)
openssl s_client -quiet -crlf -connect localhost:993
```

Ready-made scenarios in the repository: `cpp/docker/imap_test.sh` (SEARCH), `imap_build_test.sh`
(TLS + IDLE), `imap_fuzz.sh` (ASan+UBSan), `imap_interop.sh` (end-to-end SMTP→IMAP).

---

## Architecture Notes

- **Thread-per-connection** — IMAP sessions are long-lived (IDLE); each connection runs in
  its own thread. Total resources are bounded by `LOOK_IMAP_MAX_CONN`. TLS I/O is handled
  through a `thread_local`, so every command path is encrypted.
- **Minimal external runtime dependencies** — only OpenSSL (for TLS; the embedded SMTP server
  links it already). Maildir parsing, sequence management and SEARCH all live in the LOOK core.
- **Warm start** — the server comes up together with the main binary; no separate process or
  service.
