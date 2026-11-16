# LOOK — Embedded SMTP Server

LOOK includes an embedded SMTP server. No external service (Postfix, Exim) is needed.  
It is active only in `--mode http`, and only in Linux/POSIX builds (the embedded mail servers
are not part of the Windows build).

---

## Quick Start

Add to your `.env` file:

```
LOOK_SMTP_PORT=25
LOOK_SMTP_SUB_PORT=587
LOOK_SMTP_LOCAL_DOMAINS=myapp.com
LOOK_SMTP_CERT=/etc/look/mail.crt
LOOK_SMTP_KEY=/etc/look/mail.key
LOOK_SMTP_DKIM_KEY_FILE=/etc/dkim/myapp.pem
LOOK_SMTP_DKIM_SELECTOR=default
LOOK_SMTP_DKIM_DOMAIN=myapp.com
LOOK_SMTP_USER_DB=mysql://user:pass@127.0.0.1/mydb
LOOK_MAIL_DIR=/var/mail/look
```

```bash
lk-fcgi --mode http --port 7400 --workers 4
# HTTP :7400 and SMTP :25/:587 listen at the same time
```

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LOOK_SMTP_PORT` | — | MTA port (25). If empty, SMTP is disabled. An invalid value logs an error and SMTP does not start. |
| `LOOK_SMTP_SUB_PORT` | 0 | Submission port (587) — AUTH is required before `MAIL FROM` |
| `LOOK_SMTP_LOCAL_DOMAINS` | — | Comma-separated list of local domains (falls back to `LOOK_MAIL_DOMAIN`). Required for open relay prevention: with no domains configured, every unauthenticated recipient is refused. |
| `LOOK_SMTP_CERT` / `LOOK_SMTP_KEY` | — | TLS certificate and private key (PEM). When set, `STARTTLS` is offered; on the submission port AUTH and MAIL need TLS first. Also shared by the IMAP server. |
| `LOOK_SMTP_DKIM_KEY_FILE` | — | RSA private key (PEM). If not set, DKIM signing is skipped. |
| `LOOK_SMTP_DKIM_SELECTOR` | `mail` | DNS TXT record selector (`<selector>._domainkey.myapp.com`) |
| `LOOK_SMTP_DKIM_DOMAIN` | — | DKIM `d=` field (usually the main domain). If not set, DKIM signing is skipped. |
| `LOOK_SMTP_USER_DB` | — | `mysql://...` DSN. AUTH is checked against the `mail_users` table. |
| `LOOK_SMTP_AUTH_TOKEN` | — | Simple single-token auth (for testing/development): the AUTH password must equal the token. Fallback when no DB is configured. |
| `LOOK_SMTP_AUTH_DISABLED` | — | `true` refuses every AUTH attempt. |
| `LOOK_SMTP_BANNER` | `localhost` | Host name in the `220` greeting and the EHLO reply |
| `LOOK_MAIL_DIR` | `/var/mail/look` | Maildir delivery directory |
| `LOOK_SMTP_MAX_CONN` | 1000 | Maximum concurrent connections |
| `LOOK_SMTP_MAX_CONNS_IP` | 10 | Maximum concurrent connections per IP address |
| `LOOK_SMTP_MAX_MSG_SIZE` | 26214400 | Maximum message size (25 MB) |
| `LOOK_SMTP_MAX_RCPT` | 100 | Maximum recipients per message |
| `LOOK_SMTP_MAX_ERRORS` | 5 | Error limit per connection (then disconnect) |
| `LOOK_SMTP_IDLE_TIMEOUT` | 300 | Seconds a connection may stay silent before it is closed; `0` disables it |

---

## User Database (`mail_users`)

When the `LOOK_SMTP_USER_DB` DSN is set, SMTP AUTH PLAIN checks credentials against the
`mail_users` table.

```sql
CREATE TABLE mail_users (
    id       INT AUTO_INCREMENT PRIMARY KEY,
    email    VARCHAR(255) UNIQUE NOT NULL,
    password VARCHAR(255) NOT NULL,  -- pbkdf2$sha256$... (generate with auth::hash)
    active   TINYINT DEFAULT 1
);
```

Creating a user (in a LOOK route):

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

---

## Creating a DKIM Key

```bash
# Generate the key
openssl genrsa -out /etc/dkim/myapp.pem 2048
chmod 600 /etc/dkim/myapp.pem

# Get the public key for the DNS TXT record
openssl rsa -in /etc/dkim/myapp.pem -pubout -outform DER 2>/dev/null | base64 | tr -d '\n'
```

Add to DNS (`default._domainkey.myapp.com`, matching `LOOK_SMTP_DKIM_SELECTOR=default`):

```
v=DKIM1; k=rsa; p=<public_key_base64>
```

---

## TLS (STARTTLS)

- With `LOOK_SMTP_CERT` / `LOOK_SMTP_KEY` set, `STARTTLS` is advertised in the EHLO reply.
  On the submission port, `AUTH` and `MAIL FROM` are refused with
  `530 5.7.0 Must issue a STARTTLS command first` until TLS is active. TLS 1.2 is the minimum.
- Without a certificate, the submission port offers and accepts AUTH only for connections
  from the same machine (`127.0.0.1` / `::1`); other clients get
  `530 5.7.0 AUTH without encryption is accepted from this machine only (127.0.0.1 / ::1)`.
- `AUTH` is not accepted on port 25 (`503 5.5.1 AUTH not permitted on port 25`).

---

## Security Features

| Feature | Detail |
|---------|--------|
| Open relay prevention | Unauthenticated relay outside `LOOK_SMTP_LOCAL_DOMAINS` → `550 5.7.1 Relay access denied — set LOOK_SMTP_LOCAL_DOMAINS` |
| DKIM signing | If AUTH succeeded or the session is TLS, and both `LOOK_SMTP_DKIM_KEY_FILE` and `LOOK_SMTP_DKIM_DOMAIN` are set → `DKIM-Signature: v=1; a=rsa-sha256` is added |
| SPF check | Incoming messages go through an SPF check (RFC 7208); an SPF `FAIL` is logged as a warning, the message is not rejected |
| PBKDF2-HMAC-SHA256 | AUTH passwords in `pbkdf2$sha256$100000$...` format — compatible with `auth::hash()` |
| Per-IP connection limit | More than `LOOK_SMTP_MAX_CONNS_IP` connections from one IP → `421 4.3.2 Too many connections from your IP` |
| Slowloris | Silent connections are closed after `LOOK_SMTP_IDLE_TIMEOUT` seconds; a single line is capped at 64 KB (`500 5.5.2 Line too long`) |

---

## Maildir Format

Delivered messages are written in Maildir format **to each local recipient's own directory**:
`LOOK_MAIL_DIR/<recipient>/inbox/new/`. This way the embedded **IMAP server** reads each user's
INBOX from the right place (see [imap-server.md](imap-server.md)).

```
/var/mail/look/
  alice@myapp.com/
    inbox/
      new/  1751234567.abc   ← message for alice
      cur/                    ← read/flagged via IMAP STORE (:2,S)
      tmp/
  bob@myapp.com/
    inbox/ ...
```

> Because the recipient name is a directory component, unsafe recipients containing `/` `\`
> `..` or control characters are skipped during delivery (path traversal protection).

---

## Together with IMAP — The Full Mail Chain

On its own, SMTP only **delivers** (writes to the Maildir). For users to read their mail,
also enable the embedded **IMAP server**: `LOOK_IMAP_PORT=143`. Both share the same
`mail_users` table and the same Maildir — SMTP receives, IMAP serves.
Details: [imap-server.md](imap-server.md).

---

## Using with Docker

```bash
docker run -p 7400:7400 -p 25:2525 -p 587:5870 \
  -v /etc/dkim:/opt/dkim:ro \
  -v /var/mail/look:/var/mail/look \
  -e LOOK_SMTP_PORT=2525 \
  -e LOOK_SMTP_SUB_PORT=5870 \
  -e LOOK_SMTP_LOCAL_DOMAINS=myapp.com \
  -e LOOK_SMTP_DKIM_KEY_FILE=/opt/dkim/myapp.pem \
  -e LOOK_SMTP_DKIM_DOMAIN=myapp.com \
  -e LOOK_SMTP_USER_DB=mysql://user:pass@host.docker.internal/mydb \
  -e LOOK_MAIL_DIR=/var/mail/look \
  look-prod
```

> Inside a container you cannot bind to privileged ports — use `LOOK_SMTP_PORT=2525` and
> `LOOK_SMTP_SUB_PORT=5870`, and map them on the host with `iptables` or Docker `-p 25:2525`.
>
> Connections that reach the container through a port mapping do not come from `127.0.0.1`,
> so without `LOOK_SMTP_CERT` / `LOOK_SMTP_KEY` the submission port refuses AUTH. Mount a
> certificate and set both variables, or put a relay with TLS in front.

---

## Testing

```bash
# Connection test
telnet localhost 587

# AUTH PLAIN (base64: \0user@domain.com\0password)
B64=$(printf '\0user@domain.com\0mypassword' | base64 | tr -d '\n')
echo "AUTH PLAIN $B64"

# Relay prevention test
# RCPT TO an external domain → expect 550
```

With a certificate configured, test the submission port over TLS instead:
`openssl s_client -quiet -crlf -starttls smtp -connect localhost:587`.

---

## Difference from the mail:: Module

| | `mail::` | Embedded SMTP |
|--|----------|---------------|
| Method | External API (Mailgun, SendGrid, Postmark) or an SMTP relay (`MAIL_PROVIDER=smtp`) | Your own server |
| Setup | Just an API key (or relay credentials) | Requires DNS MX, DKIM and PTR records |
| Usage | `mail::send(to, subject, body)` | Incoming mail delivery + outgoing AUTH |
| Best for | Transactional e-mail | Those who want to run a mail server |

The two can be used together: `mail::` for outgoing mail, the embedded SMTP server for incoming.
