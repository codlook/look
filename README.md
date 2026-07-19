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

- 🪶 **Simple to deploy** — a single binary. Copy it to a VPS, systemd, Plesk or
  XAMPP and run — no `composer`/`npm` install step, no package tree.
- ⚡ **Fast** — **~9,800 req/s** on a direct port. On a real DB-bound workload
  (QR-menu JOIN over 50k rows, 1 CPU / 1 GB, best-of-3): **LOOK 2,837 req/s vs
  PHP 8.3 + JIT + FPM 2,184** — and **9–29 MB RAM against PHP's 43–47 MB**
  (3–5× less), with the lowest CPU per request. Method: same host, same schema and
  query, `CPUQuota=100%` + `MemoryMax=1G` on both, equal pool sizes (32), no TLS,
  `c=100`, best-of-3 runs (a single run is ~2× noisy). PHP ran under FPM, not
  `php -S` — the built-in server is single-process and would have understated it.
- 🔋 **Batteries included** — MySQL / PostgreSQL / SQLite, sessions, JWT, cache,
  queue, background jobs, an embedded **SMTP + IMAP** mail server, WebSocket & SSE —
  all built in.
- 🧩 **Ergonomic** — `fn() => …` arrows, `$row.col` access, `module::method`,
  no required semicolons.

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

## Known limits (worth knowing up front)

- **One entry file.** There is no project-local `include`/`require`; `use` resolves
  built-ins and installed modules (`~/.look/modules`), not your own files. So your
  routes live in one `.lk` file — but **views belong in `views/*.html`** via the
  template engine, and settings in a config file, which is what keeps it readable.
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
