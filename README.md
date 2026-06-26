 # ◯ LOOK Language

 
LOOK is an open-source web scripting language written in C++23, designed as a modern alternative to PHP.  
Built-in routing. Zero framework needed. Drop it on Apache/nginx and go.

[![VS Code](https://img.shields.io/visual-studio-marketplace/v/codlook.look-lang?label=VS%20Code&color=3b82f6)](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang)
[![License](https://img.shields.io/badge/license-Apache%202.0-green)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-blue)](https://github.com/Codlook/look/releases)

---

## Quick Start

```lk
$conn = db::connect("mysql://root:@127.0.0.1/blog");

route("GET", "/", function() use ($conn) {
    $rows = db::query($conn, "SELECT * FROM posts ORDER BY id DESC LIMIT 10", []);
    print(json::encode(["ok" => true, "data" => $rows]));
});

route("POST", "/post", function() use ($conn) {
    $data = request::json();
    db::exec($conn, "INSERT INTO posts (title, body) VALUES (?,?)",
             [$data["title"], $data["body"]]);
    print(json::encode(["ok" => true, "id" => db::last_id($conn)]));
});

route("404", function() {
    response::status(404);
    print(json::encode(["ok" => false, "error" => "Not found"]));
});
```

Save as `htdocs/index.lk`, start look-fcgi — done.

---

## Install

### Linux — One Command

```bash
curl -fsSL https://get.codlook.com | sudo bash
```

Ubuntu 22/24, AlmaLinux 8, Debian 11/12. x86_64 + ARM64.

### Windows + XAMPP

```batch
install.bat --xampp C:\xampp
```

### Plesk Extension

Plesk → Extensions → Upload ZIP → `look-lang-1.0.0.zip`  
Add domain → Start → done. No terminal, no SSH.

### Docker

```bash
# 5 seconds to run
docker run -p 9000:9000 \
  -v $(pwd):/app \
  -e LOOK_MODE=http \
  codlook/look:latest

curl http://localhost:9000/
# {"ok":true,"message":"LOOK is running!","version":"1.0.0"}
```

Or with `docker-compose.yml` in your project:
```bash
docker compose up
```

### Verify

```bash
look version       # LOOK 1.0.0 (linux/amd64)
look-fcgi version  # LOOK 1.0.0 (linux/amd64)
```

---

## Performance

Real-world benchmarks on [looktest.tobiyo.com.tr](https://looktest.tobiyo.com.tr) — 29 routes, MariaDB, Plesk/AlmaLinux 8:

| Mode | Endpoint | RPS | Latency | Memory | Notes |
|------|----------|-----|---------|--------|-------|
| fcgi (32w) | DB — 17 routes | **7,846** | 27ms | **8 MB** | 10min / 4.7M req / 0 errors |
| http+VM (32w) | /router (no DB) | **8,276** | 0.12ms | 8 MB | Apache bypass |
| http+VM (32w) | /compute (fib40) | **8,685** | 0.12ms | 8 MB | VM 7.8× speedup |
| http+VM (c=1000) | /router peak | **7,886** | 0.13ms | 8 MB | 0 errors |

**Apache on same machine: ~1.8 GB RAM. LOOK: 8 MB.**

### Measured Numbers (AlmaLinux 8, 32 workers, looktest.tobiyo.com.tr)

| Metric | Result | Condition |
|--------|--------|-----------|
| RPS — router (no DB) | **8,276** | c=100, Apache bypass |
| RPS — compute (fib40) | **8,685** | VM 7.8× speedup |
| RPS — peak (c=1000) | **7,886** | 0 errors |
| Latency | **0.12ms** | router endpoint |
| Memory (32 workers) | **8 MB** | vs Apache ~1.8 GB |
| Sustained load | **4.7M requests** | 10 min, 0 errors |

> PHP / Node.js / Go comparison on same hardware — coming soon.

---

## Binaries

| Binary | Purpose |
|--------|---------|
| `look-fcgi` | FastCGI server — port 9000 (production) |
| `look-fcgi --mode http` | HTTP/1.1 server — Apache bypass, WebSocket, SSE |
| `look-cgi` | CGI — fallback / standalone |
| `look` | CLI — `look test`, `look repl`, `look install`, `look version` |

---

## Language Features

| Feature | Status |
|---------|--------|
| Types: int, float, string, bool, null, array, assoc | ✅ |
| Routing — `route("GET", "/path/{id}", fn)` | ✅ |
| DB: MySQL · SQLite · PostgreSQL — zero dependencies | ✅ |
| struct (Go style) + const iota | ✅ |
| switch (Go style — no break, multi-case) | ✅ |
| `parallel()` + `channel()` — Go-style goroutines | ✅ |
| WebSocket — `route("WS", ...)`, `ws::send/broadcast` | ✅ |
| SSE — `route("SSE", ...)` + `timer::after/every/cancel` | ✅ |
| Concurrent runtime — `--workers N`, ThreadPool | ✅ |
| `--mode http` — epoll (Linux), IOCP (Windows) | ✅ |
| Bytecode VM — 7.8× compute speedup | ✅ |
| `use "file.lk"` module system | ✅ |
| Upload: `request::file()`, magic byte, SHA-256 | ✅ |
| template:: — layout inheritance, partial include | ✅ |
| cache:: — in-memory TTL, thread-safe | ✅ |
| queue:: — named FIFO, cross-request | ✅ |
| jobs:: — SQLite persistent queue, retry, delayed, worker | ✅ |
| mail:: — Mailgun / SendGrid / Postmark | ✅ |
| `look test` runner + `look repl` + `look install` | ✅ |

---

## Modules

```lk
# Core (no import needed)
route()  request::  response::  db::  json::
session::  cookie::  log::  file::  date::

# Standard (with use X;)
use math;      # sqrt, pow, random, sin, cos...
use string;    # upper, lower, trim, split, replace, slugify...
use array;     # sort, filter, map, reduce, find, any, all...
use type;      # of, is_int, is_string, to_int, to_float...
use auth;      # hash(), verify() — PBKDF2-SHA256
use validator; # required, email, min, max, in
use html;      # escape, strip
use template;  # render($path, $data) — layout, partial
use cache;     # set/get/has/delete — in-memory TTL
use queue;     # push/pop/peek — named FIFO, cross-request
use jobs;      # push/next/done/fail/worker/run — SQLite job queue
use mail;      # send/send_html — Mailgun, SendGrid, Postmark
```

---

## Code Examples

### WebSocket Chat

```lk
$hub = channel();

route("WS", "/chat", function($ws) use ($hub) {
    parallel(function() use ($ws, $hub) {
        while (true) {
            $msg = receive($hub);
            if ($msg == null) { break; }
            ws::send($ws, $msg);
        }
    });

    ws::on($ws, "message", function($data) use ($hub) {
        send($hub, json::encode([
            "from" => request::ip(),
            "msg"  => json::decode($data)["msg"]
        ]));
    });
});
```

### Parallel DB Queries

```lk
route("GET", "/dashboard", function() use ($conn) {
    $ch = channel(3);

    parallel(function() use ($ch, $conn) {
        send($ch, db::col($conn, "SELECT count(*) FROM users", []));
    });
    parallel(function() use ($ch, $conn) {
        send($ch, db::col($conn, "SELECT count(*) FROM orders", []));
    });
    parallel(function() use ($ch, $conn) {
        send($ch, db::col($conn, "SELECT sum(amount) FROM orders", []));
    });

    print(json::encode([
        "users"   => receive($ch),
        "orders"  => receive($ch),
        "revenue" => receive($ch)
    ]));
});
```

### Background Jobs

```lk
use jobs;
use mail;

route("POST", "/register", function() use ($conn) {
    $data = request::json();
    db::exec($conn, "INSERT INTO users (email) VALUES (?)", [$data["email"]]);
    jobs::push("welcome", ["to" => $data["email"]], 3, 0);
    print(json::encode(["ok" => true]));
});

# worker.lk — separate process
jobs::recover("welcome");
jobs::worker("welcome", function($job) {
    $r = mail::send($job["payload"]["to"], "Welcome!", "Your account is ready.");
    return $r["ok"];
});
jobs::run(5000);
```

---

## Live Demo

**[looktest.tobiyo.com.tr](https://looktest.tobiyo.com.tr)** — QR Menu app, 29 routes, MariaDB, running on Plesk/AlmaLinux 8.

---

## Supported Platforms

| Platform | Status |
|----------|--------|
| Ubuntu 22.04 / 24.04 | ✅ |
| AlmaLinux 8 / RHEL 8 | ✅ |
| Debian 11 / 12 | ✅ |
| Windows 10/11 + XAMPP | ✅ |
| Plesk (AlmaLinux 8) | ✅ Live |

## Supported Databases

| DB | DSN |
|----|-----|
| MySQL / MariaDB | `mysql://user:pass@127.0.0.1/db` |
| SQLite | `sqlite://./data.db` |
| PostgreSQL | `postgres://user:pass@127.0.0.1:5432/db` |

---

## Build

### Windows

```powershell
cd cpp\build
cmake --build . --config Release
```

### Linux (Docker)

```bash
cd cpp

# Ubuntu 24.04
docker build -f Dockerfile.linux-build -t look-linux-builder .
docker run --rm -v "${PWD}:/src" look-linux-builder \
  sh -c "cd /src/build-linux && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4"

# AlmaLinux 8 / Plesk
docker build -f Dockerfile.almalinux8-build -t look-almalinux8-builder .
docker run --rm -v "${PWD}:/src" look-almalinux8-builder \
  sh -c "mkdir -p /src/build-almalinux8 && cd /src/build-almalinux8 && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4"
```

---

Developer
Codlook Bilişim — Diyarbakır, Türkiye

LOOK programlama dili, Diyarbakır'da faaliyet gösteren yazılım şirketi Codlook Bilişim tarafından geliştirilmektedir.

## Links

- 🌐 Website: [codlook.com](https://codlook.com)
- 📖 Docs: [codlook.com/docs](https://codlook.com/docs)
- 🧩 VS Code: [Marketplace](https://marketplace.visualstudio.com/items?itemName=codlook.look-lang)
- 🐦 Twitter/X: [@codlook](https://twitter.com/codlook)

---

## License

Apache 2.0 © 2026 [Codlook](https://codlook.com)

"LOOK", "look-lang", and "Codlook" are trademarks of Codlook.  
Applications you write with LOOK remain entirely your own property.

---

**v1.0.0** · C++23 · CMake 3.20+ · Apache 2.4+ / nginx
