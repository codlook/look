// packages.codlook.com — built-in, module and package data
// Updated 2026-07-18. Verified against the language source (cpp/src/builtins.cpp)
// and the actual look-modules / look-packages repositories.
//
// Three categories:
//   builtin  — ships inside the language, NO installation
//   modules  — installable with `lk module install …`  (look-modules)
//   packages — installable with `lk install …`         (look-packages)
//
// NOTE: http, crypto, mail, cache and queue used to be listed here as installable
// modules. They are NOT — they are built into the language, and those install
// commands pointed at repositories that do not exist. Fixed below.

const PACKAGES_DATA = {

  // ── Built into the language — no install needed ────────────────────────────
  "builtin": [
    {
      "name": "http",
      "description": "Outbound HTTP client — GET/POST/PUT/DELETE/PATCH/HEAD, JSON bodies, custom headers, timeouts, streaming responses. Fiber-aware: while waiting on a slow external API the worker keeps serving other requests.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "use http",
      "tags": ["http", "api", "client", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🌐",
      "color": "blue"
    },
    {
      "name": "crypto",
      "description": "SHA-256, HMAC-SHA256, base64 and base64url, hex encode/decode, constant-time compare, secure random bytes/strings, UUID, RS256 sign/verify for JWT.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "use crypto",
      "tags": ["crypto", "hash", "security", "uuid", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🔒",
      "color": "purple"
    },
    {
      "name": "db",
      "description": "MySQL/MariaDB, PostgreSQL and SQLite — wire protocol implemented inside the language, no external driver. Connection pool, parameterised queries, transactions with commit/rollback and savepoints.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "db::connect(\"mysql://user:pass@host/db\")",
      "tags": ["database", "mysql", "postgres", "sqlite", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🗄️",
      "color": "green"
    },
    {
      "name": "template",
      "description": "View engine with layout inheritance — {$var} escaped output, {!$var} raw, {#if}…{#else}, {#each $list as $item}…{#empty}, {#extends}, {#block}, {#include}. Keeps HTML out of your .lk files.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "use template",
      "tags": ["template", "view", "html", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🧩",
      "color": "cyan"
    },
    {
      "name": "session",
      "description": "Sessions backed by files or Redis/RESP2, plus cookie helpers — start / get / set / destroy / regenerate for session rotation after login.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "session::start()",
      "tags": ["session", "cookie", "auth", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🎫",
      "color": "orange"
    },
    {
      "name": "cache",
      "description": "In-process key/value cache with TTL and a bounded entry cap (LOOK_CACHE_MAX_ENTRIES) so it cannot grow without limit — get / set / has / delete / flush / keys / size.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "cache::set(\"key\", $v, 300)",
      "tags": ["cache", "performance", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "⚡",
      "color": "yellow"
    },
    {
      "name": "queue",
      "description": "Lightweight in-process job queue — push / pop / size / peek / names / clear. Pairs with jobs:: for background workers.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "queue::push(\"mail\", $job)",
      "tags": ["queue", "jobs", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "📬",
      "color": "blue"
    },
    {
      "name": "mail",
      "description": "Send mail plus an embedded SMTP + IMAP server — ports 25/587/465 and 143/993, STARTTLS, DKIM signing, Maildir delivery. A full mail stack inside the language.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "mail::send($to, $subject, $body)",
      "tags": ["mail", "smtp", "imap", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "✉️",
      "color": "red"
    },
    {
      "name": "validator",
      "description": "Input validation — required, email (254-char cap), integer/numeric with strict full-string parsing (\"123abc\" is rejected, not silently truncated), min/max and a combined check().",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "validator::email($v)",
      "tags": ["validation", "security", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "✅",
      "color": "green"
    },
    {
      "name": "ws",
      "description": "WebSocket and Server-Sent Events (SSE) — upgrade handling, broadcast, per-connection state. The broadcast snapshot pattern is race-free (verified under TSan).",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "ws::broadcast($msg)",
      "tags": ["websocket", "sse", "realtime", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "🔌",
      "color": "cyan"
    },
    {
      "name": "jobs",
      "description": "Background jobs — push / next / done / fail / retry / purge / recover / stats. Runs outside the request cycle, survives worker restarts.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "jobs::push(\"queue\", $payload)",
      "tags": ["jobs", "background", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "⚙️",
      "color": "orange"
    },
    {
      "name": "file",
      "description": "Sandboxed file access — read / put / append / exists / remove / size / store and an upload directory, with separator-bounded containment so paths cannot escape the root.",
      "author": "core",
      "github": "https://codlook.com/docs",
      "install": "Built-in — no installation required",
      "use": "file::read(\"config/app.json\")",
      "tags": ["file", "io", "security", "builtin"],
      "version": "core",
      "approved": true,
      "icon": "📁",
      "color": "yellow"
    }
  ],

  // ── Installable modules — lk module install … ──────────────────────────────
  "modules": [
    {
      "name": "jwt",
      "description": "Create, verify and decode JSON Web Tokens — jwt_sign / jwt_verify / jwt_decode, HS256 and HS512, usable with a middleware pattern.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/jwt",
      "install": "lk module install github.com/codlook/look-modules/jwt",
      "use": "use jwt",
      "tags": ["auth", "token", "security"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔐",
      "color": "purple"
    },
    {
      "name": "ai",
      "description": "Official Claude (Anthropic) module — ai_chat() for a complete reply, ai_stream() for token-by-token streaming, with system prompts, prompt caching, model and max_tokens options. Pure LOOK, built on the core http:: primitives.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/ai",
      "install": "lk module install github.com/codlook/look-modules/ai",
      "use": "use ai",
      "tags": ["ai", "claude", "anthropic", "llm"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🤖",
      "color": "cyan"
    }
  ],

  // ── Installable packages — lk install … ────────────────────────────────────
  "packages": [
    {
      "name": "firebase",
      "description": "Firebase integration — Firestore CRUD, Authentication (email/password, token verification) and Realtime Database. Access Google Firebase services directly from LOOK.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-packages/tree/main/firebase",
      "install": "lk install github.com/codlook/look-packages/firebase",
      "use": "use \"pkg/firebase\"",
      "tags": ["firebase", "database", "auth", "google"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔥",
      "color": "orange"
    },
    {
      "name": "iyzico",
      "description": "iyzico payment integration for Turkish e-commerce — payment creation, 3D Secure flow, refund and cancellation, HMAC request signing. Drops straight into a checkout route.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-packages/tree/main/iyzico",
      "install": "lk install github.com/codlook/look-packages/iyzico",
      "use": "use \"pkg/iyzico\"",
      "tags": ["payment", "iyzico", "ecommerce", "3dsecure"],
      "version": "1.0.0",
      "approved": true,
      "icon": "💳",
      "color": "green"
    },
    {
      "name": "monitor",
      "description": "Runtime observability — formats runtime::stats() into Prometheus (/metrics) and JSON, with correct counter/gauge typing so Grafana rate() works. Reads the core counters (requests, 5xx, latency, DB pool, VM-disabled routes). Safe-by-default: importing it registers no routes on its own.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-packages/tree/main/monitor",
      "install": "lk install github.com/codlook/look-packages/monitor",
      "use": "use \"monitor/monitor.lk\"",
      "tags": ["monitoring", "observability", "prometheus", "metrics"],
      "version": "1.0.0",
      "approved": true,
      "icon": "📊",
      "color": "blue"
    }
  ]
};
