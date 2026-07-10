# LOOK

**LOOK** is a fast, **zero-dependency** web scripting language written in C++23.
It is designed for the web: routing, databases, sessions, JWT, validation,
caching, a mail server, WebSockets and SSE all ship **inside the language** and
compile to a single self-contained binary. No framework to set up, no package
tree, no separate runtime.

```lk
use jwt

# Register a shared service once — no capture (use) needed in routes
app::set("db", db::connect("mysql://root:@127.0.0.1/mydb"))

route("GET", "/products", function() {
    $rows = db::query(app::db(), "SELECT * FROM products", [])
    response::json(["ok" => true, "data" => $rows])
})

# One-line route — arrow function
route("GET", "/ping", fn() => response::json(["ok" => true]))

# Path params + auth
route("POST", "/login", function() {
    $body  = request::json()
    $token = jwt_sign(["id" => $body.id], env("JWT_SECRET"))
    response::json(["token" => $token])
})

route("404", fn() => response::error(404, "Not found"))
```

> **Syntax note:** `;` is optional (canonical style: omit it). Use `fn` instead of
> `function`, and `$row.col` dot access instead of long bracket indexing.

---

## Why LOOK

- 🪶 **Zero dependency** — one static binary. Copy it to a VPS, systemd, Plesk or
  XAMPP and run. No `composer`/`npm`, no dependency tree, no surprises.
- ⚡ **Fast** — FastCGI warm start; **~9,800 req/s** direct port, **28 MB RAM steady
  over 1M+ requests** (no leak).
- 🔋 **Batteries included** — MySQL / PostgreSQL / SQLite, sessions, JWT, cache,
  queue, background jobs, an embedded **SMTP + IMAP** mail server, WebSocket & SSE —
  all built in, zero third-party libraries.
- 🧩 **Ergonomic** — `fn() => …` arrows, `$row.col` access, `module::method`,
  no required semicolons.

## Features

| Feature | |
|---|---|
| Routing — GET/POST/PUT/DELETE, path params, `404`, WebSocket, SSE | ✅ |
| Databases — MySQL/MariaDB, PostgreSQL (wire protocol), SQLite | ✅ |
| Sessions (file or Redis/RESP2), cookies, JWT, validation, templates | ✅ |
| Concurrency — `parallel()` + channels; FastCGI multi-worker | ✅ |
| Embedded SMTP server (25/587/465, STARTTLS, DKIM) | ✅ |
| Embedded IMAP server (143/993, STARTTLS, SEARCH, IDLE) | ✅ |
| `crypto::` — SHA-256, HMAC, base64url, UUID, secure random | ✅ |
| Rate limiting (token bucket, per-IP + global), file sandbox | ✅ |
| Test runner + REPL — `lk test` · `lk repl` · `lk --check` (parse-only) | ✅ |

## Build

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j
# → lk (CLI) · lk-fcgi (FastCGI/HTTP server) · lk-cgi (CGI)
```

Requires a C++23 compiler and CMake 3.20+. OpenSSL is used for TLS; the release
binaries bundle it statically for a truly dependency-free build.

## Install (prebuilt)

Grab a package from **[Releases](https://github.com/codlook/look/releases)**:

| Platform | Package |
|---|---|
| Linux (Ubuntu / AlmaLinux / RHEL) | `look-lang-linux-1.0.0.zip` → `sudo bash install.sh` |
| AlmaLinux / RHEL (dnf) | `look-lang-1.0.0-1.el8.x86_64.rpm` |
| Plesk panel | `look-lang-plesk-1.0.0.zip` → `plesk bin extension --install …` |
| Windows + XAMPP | `look-lang-xampp-1.0.0.zip` → `.\install.ps1` |
| VS Code editor support | [Marketplace](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang) |

## Ecosystem

This repository is **the LOOK language core** only. Everything else lives on its own:

| | Where |
|---|---|
| Prebuilt binaries + VS Code extension | [Releases](https://github.com/codlook/look/releases) · [Marketplace](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang) |
| Modules (jwt, http, crypto, mail…) | [github.com/codlook/look-modules](https://github.com/codlook/look-modules) |
| Packages | [github.com/codlook/look-packages](https://github.com/codlook/look-packages) |
| Package & module directory | [packages.codlook.com](https://packages.codlook.com) |
| Documentation | [codlook.com/docs](https://codlook.com/docs) |

## Security

See [SECURITY.md](SECURITY.md). Zero third-party dependencies mean the entire
attack surface is auditable; parsers are hardened and fuzzed (ASan/UBSan/TSan),
with regression tests run on every build.

## License

Apache 2.0 © 2026 [Codlook](https://codlook.com)

"LOOK", "look-lang" and "Codlook" are trademarks of Codlook.
Applications you write with LOOK remain entirely your own property.

---

Made by **Codlook Bilişim** — Diyarbakır, Türkiye · [codlook.com](https://codlook.com) · [@codlook](https://twitter.com/codlook)
