# LOOK

**LOOK** is a fast web scripting language written in C++23. It is designed for
the web: routing, databases, sessions, JWT, validation, caching, a mail server,
WebSockets and SSE all ship **inside the language**. No framework to set up, no
separate runtime — write a `.lk` file and run it.

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

- 🪶 **Simple to deploy** — a single binary. Copy it to a VPS, systemd, Plesk, Docker or a
  bare Windows box and run — no `composer`/`npm` install step, no package tree. On Windows,
  `lk-fcgi.exe --mode http` is a complete web server: no Apache, no XAMPP, no config.
- ⚡ **Fast** — **~9,800 req/s** on a direct port. On a real DB-bound workload
  (QR-menu JOIN over 50k rows, 1 CPU / 1 GB, best-of-3): **LOOK 2,837 req/s vs
  PHP 8.3 + JIT + FPM 2,184** — and **9–29 MB RAM against PHP's 43–47 MB**
  (3–5× less), with the lowest CPU per request. Method: same host, same schema and
  query, `CPUQuota=100%` + `MemoryMax=1G` on both, equal pool sizes (32), no TLS,
  `c=100`, best-of-3 runs (a single run is ~2× noisy). PHP ran under FPM, not
  `php -S` — the built-in server is single-process and would have understated it.
- 🔋 **Batteries included** — MySQL / MariaDB / PostgreSQL / SQLite, sessions, JWT, cache,
  queue, background jobs, an embedded **SMTP** server (+ Milestone-1 IMAP), WebSocket & SSE —
  all built in.
- 🧩 **Ergonomic** — `fn() => …` arrows, `$row.col` access, `module::method`,
  clean `use` module imports.

## See it running

Five real applications, all served by a **single** LOOK process, each with its own
database — **[test.codlook.com](https://test.codlook.com)**:

| | |
|---|---|
| **/blog** | Posts, admin panel, categories, cover-image upload |
| **/qrmenu** | Visual restaurant menu **+ REST API + a route listing page** |
| **/chat** | Nickname + emoji, near-instant messaging |
| **/products** | Products, categories, brands, variants, extra attributes — create & edit |
| **/ai** | Claude API with in-memory conversation (deliberately **no** database) |

They are laid out the way a real project should be — logic separate from markup:

```
index.lk          # router: config + routes; each route renders a view
config/app.json   # settings (DSNs, limits) — outside the code
views/layout.html # base layout  {#block content}
views/blog/…      # one template per page
```

```lk
route("GET", "/blog", function() use ($db) {
    $posts = db::query($db, "SELECT slug,title FROM posts ORDER BY id DESC", [])
    response::html(template::render("views/blog/index", ["posts" => $posts]))
})
```
```html
{#extends "views/layout"}
{#block content}
  {#each posts as p}<h2><a href="/blog/{$p.slug}">{$p.title}</a></h2>{#empty}No posts.{/each}
{/block}
```

## Project layout

LOOK is **not a one-file language**. A `.lk` file is a program you can run on its own
(`lk report.lk`), and a real project is split the way you would expect — the entry file
only wires things together:

```
myapp/
├── index.lk              # entry: routes + service wiring
├── config/app.json       # settings
├── views/                # templates — {$var}, {#if}, {#each}, {#extends}
│   ├── layout.html
│   └── product.html
└── ~/.look/modules/      # your own modules, loaded with `use`
    ├── model/model.lk
    └── util/util.lk
```

```lk
use model                 # ~/.look/modules/model/model.lk
use util
use template

app::set("db", db::connect(env("DB_DSN")))     # register once, use anywhere

route("GET", "/product/{id}", function() {
    $p = product_find(request::param("id"))     # from model — plain name, no prefix
    return response::html(template::render("views/product", ["p" => $p]))
})
```

Three things worth knowing:

- **Your module functions are called by their plain name** (`product_find`), not
  `model::product_find`. The `mod::fn` form is for built-ins (`string::upper`).
- **`app::set` / `app::get`** share services (DB handle, config) across routes without
  passing them around or capturing them with `use`.
- **Each file still runs standalone** — a module or a script can be executed directly,
  which makes CLI tools and cron jobs trivial (`lk tools/import.lk`).

## Features

| Feature | |
|---|---|
| Routing — GET/POST/PUT/DELETE, path params, `404`, WebSocket, SSE | ✅ |
| Databases — MySQL/MariaDB, PostgreSQL (wire protocol), SQLite | ✅ |
| Sessions (file or Redis/RESP2), cookies, JWT, validation, templates | ✅ |
| Concurrency — `parallel()` + channels; FastCGI multi-worker | ✅ |
| Embedded SMTP server (25/587/465, STARTTLS, DKIM sign **and** verify, SPF check) | ✅ |
| Embedded IMAP server — **Milestone 1**: read-only INBOX (see note below) | 🟡 |
| `crypto::` — SHA-256, HMAC, base64url, UUID, secure random | ✅ |
| Rate limiting (token bucket, per-IP + global), file sandbox | ✅ |
| Test runner + REPL — `lk test` · `lk repl` · `lk --check` (parse-only) | ✅ |

### Tested database versions

These are exercised against **real servers** in CI (`cpp/tests/db_latest_test.sh`
tracks `latest`, `mysql_auth_test.sh` pins the matrix) — not assumed:

The four relational engines are **built into the core** — the wire protocol is
implemented inside the language, no external driver:

| Engine | Verified | Auth |
|---|---|---|
| MySQL | 5.7 · 8.0 · 8.2 · 8.4 · 9.x | `caching_sha2_password` (incl. RSA full-auth over a non-TLS socket) **and** `mysql_native_password` — the plugin the server advertises is honoured, so 8.4 (native *disabled*) and 9.x (native *removed*) work |
| MariaDB | 10.11 · 11.4 · 12.x | `mysql_native_password` |
| PostgreSQL | 14 · 18 | SCRAM-SHA-256 / md5 (protocol 3.0). **TLS supported** — `postgresqls://` (or `?tls=verify`) connects to managed hosts that require it (Supabase, Neon, Heroku, Azure, AWS RDS). Unlike MySQL/Redis, PostgreSQL **verifies the certificate + hostname by default** (`?tls=insecure` opts out). |
| SQLite | 3.47.2 (embedded) | file format is forward/backward compatible since 2004 |

> **Connecting to a remote database?** MySQL/MariaDB and Redis connect in **plaintext by
> default** — use `mysqls://` / `rediss://` to encrypt and add `?tls=verify` to authenticate
> the server (without `verify` the channel is encrypted but MITM is still possible).
> **PostgreSQL is the exception: `postgresqls://` (or `?tls=verify`) encrypts *and* verifies
> the certificate + hostname by default** — add `?tls=insecure` only to accept a self-signed
> cert. On loopback / a private network none of this applies.
> Full detail: [SECURITY.md → Known limitations](SECURITY.md#known-limitations).

Beyond the core, **Firebase** is available as an installable package (Firestore
CRUD, Authentication, Realtime Database) — see Ecosystem below. Payments (**iyzico**)
ship the same way. The core stays zero-dependency; optional integrations are opt-in.

Windows builds link no OpenSSL by design (zero DLLs), so MySQL
`caching_sha2_password` **full** auth is unavailable there — create the user with
`mysql_native_password`, or use the Linux build. LOOK says so explicitly instead of
failing obscurely.

## Known limits (worth knowing up front)

- **Modules live in the module directory, not in relative paths.** There is no
  `include "./lib/foo.lk"` — you split code into *modules* (see **Project layout**
  above), which `use` loads from `~/.look/modules/`. One entry file starts the app;
  everything else can be a module, a view, or config.
- **Uploads are sniffed, not trusted.** `request::file` reports the MIME from the
  file's magic bytes, not from the client's `Content-Type` — a `.php` renamed to
  `.png` is reported for what it is. Unknown/plain content comes back as
  `application/octet-stream`.
- **Persistent-process model.** Unlike PHP-FPM there is no per-request reset: a
  segfault takes the worker down and global state is shared, so it must be
  thread-safe. That is the trade for the speed and the low memory.
- **Strict, not forgiving.** LOOK sits in the Go/Node band, not the PHP one: an
  undefined variable and an out-of-range index (`$a[99]`, and `$a[99] = "x"`) are
  **errors**, not a silent `null` or a silently skipped write. Negative indices count
  from the end (`$a[-1]`). Missing *keys* are different from missing *slots*: an
  absent assoc key returns `null`, and accessors take a default —
  `request::get("page", 1)`, like `env(key, default)`.
- **Mail servers are opt-in and unequal in maturity.** Neither listens unless you
  set `LOOK_SMTP_PORT` / `LOOK_IMAP_PORT`.
  **SMTP is production-shaped**: relay is denied to unauthenticated senders, `SIZE`
  and recipient caps are *enforced* (not just advertised), SPF is checked and DKIM
  is both signed and verified — but there is no greylisting or spam scoring, so
  put it behind a filter if you accept mail from the open internet.
  **IMAP is Milestone 1**: it serves a read-only INBOX (SELECT/FETCH/SEARCH/STORE/
  EXPUNGE/APPEND/IDLE) and is meant as a webmail backend. It has **no persistent
  UIDs**, so `UID`, `CREATE`, `DELETE` and `RENAME` are answered with an explicit
  `NO [CANNOT] … (Milestone 1)` rather than wrong data. Standard desktop/mobile
  mail clients (Thunderbird, Outlook, iOS Mail) rely on `UID` and will **not** work
  against it yet.
- **Nested transactions:** `db::begin/commit/rollback` are *flat* — a second
  `db::begin` is not a nesting level (MySQL and PostgreSQL disagree on what it means).
  For nested / partial rollback use `db::transaction($c, fn)`, which uses real
  `SAVEPOINT`s and behaves the same on every engine.

## Build

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j
# → lk (CLI) · lk-fcgi (FastCGI/HTTP server) · lk-cgi (CGI)
```

Requires a C++23 compiler and CMake 3.20+.

Two things that will otherwise cost you an afternoon:

- **Target names are not the binary names.** To build one thing, use
  `--target look` (→ `lk`), `--target look-fcgi` (→ `lk-fcgi`),
  `--target look-cgi` (→ `lk-cgi`). `--target lk` fails with
  *“No rule to make target”*.
- **Static OpenSSL is the default** (`LOOK_STATIC_SSL=ON`) so the release binary is
  self-contained. That needs `libssl.a`/`libcrypto.a`, which AlmaLinux/RHEL
  `openssl-devel` does **not** ship — a fresh configure there fails in
  `find_package(OpenSSL)`. Either install static OpenSSL, or configure with
  `-DLOOK_STATIC_SSL=OFF` for a dynamically linked build.

## Install (prebuilt)

Grab a package from **[Releases](https://github.com/codlook/look/releases)**:

| Platform | Package |
|---|---|
| Linux (Ubuntu / AlmaLinux / RHEL) | `look-lang-linux-1.0.0.zip` → `sudo bash install.sh` |
| AlmaLinux / RHEL (dnf) | `look-lang-1.0.0-1.el8.x86_64.rpm` |
| Plesk panel | `look-lang-plesk-1.0.0.zip` → `plesk bin extension --install …` |
| Docker (any OS) | `docker run -p 7400:7400 codlook/look` |
| Windows | `look-lang-windows-1.0.0.zip` → unzip → `lk-fcgi.exe --mode http --port 8080` (no installer) |
| VS Code editor support | [Marketplace](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang) |

> **Platform tiers:** Linux x86_64 is **Tier 1** (full test suite gates every release; the production
> target). **Docker** runs the same environment anywhere. **Windows is Tier 2:** a plain-binary zip
> (`lk.exe` + `lk-fcgi.exe`, **no installer** — `lk-fcgi.exe --mode http` is a complete web server, no
> Apache/XAMPP), with the core test suite verified manually before each release. TSan/fuzz/differential
> and UBSan are not run on Windows (those tools do not exist for MSVC).
> See [SECURITY.md → Platform support tiers](SECURITY.md#platform-support-tiers).
>
> **Running behind Apache / nginx / IIS?** LOOK is a standard **CGI / FastCGI binary** — it wires into
> any web server (XAMPP, Laragon, WAMP, MAMP, plain Apache/nginx) with ordinary CGI configuration; the
> stack does not matter. LOOK ships the binary, not per-stack installers. See the deployment docs.

## Ecosystem

This repository is **the LOOK language core** only. Everything else lives on its own:

| | Where |
|---|---|
| Prebuilt binaries + VS Code extension | [Releases](https://github.com/codlook/look/releases) · [Marketplace](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang) |
| Modules (jwt, http, crypto, mail…) | [github.com/codlook/look-modules](https://github.com/codlook/look-modules) |
| Packages (Firebase, iyzico…) | [github.com/codlook/look-packages](https://github.com/codlook/look-packages) |
| Package & module directory | [packages.codlook.com](https://packages.codlook.com) |
| Documentation | [codlook.com/docs](https://codlook.com/docs) |

## Project status

See [BUG_AVI_HARITASI.md](BUG_AVI_HARITASI.md) (Turkish) for the honest, up-to-date
engineering log: the systematic bug-hunt map, every bug we closed and **why it hid for
so long**, the bug-class catalogue, and the rules we work by. Each fix lands with a
regression guard, so the list doubles as a record of what is now locked down.

## Security

See [SECURITY.md](SECURITY.md). The protocol parsers are our own code and kept
in one place, so the attack surface is auditable; parsers are hardened and fuzzed
(ASan/UBSan/TSan), with regression tests run on every build.

## License

Apache 2.0 © 2026 [Codlook](https://codlook.com)

"LOOK", "look-lang" and "Codlook" are trademarks of Codlook.
Applications you write with LOOK remain entirely your own property.

---

Made by **Codlook Bilişim** — Diyarbakır, Türkiye · [codlook.com](https://codlook.com) · [@codlook](https://twitter.com/codlook)
