# LOOK Language

> PHP kadar kolay dağıtılır, Go kadar sade yazılır, C++ kadar hızlı çalışır.

LOOK, C++23 ile yazılmış Apache/CGI+FastCGI uyumlu bir web scripting dilidir.
PHP'ye alternatif olarak tasarlanmıştır — routing dile gömülü, framework kurma zorunluluğu yok.

---

## Hızlı Başlangıç

```lk
$conn = db::connect("mysql://root:@127.0.0.1/mydb");

route("GET", "/", function() use ($conn) {
    $rows = db::query($conn, "SELECT * FROM urunler", []);
    print(json::encode(["ok" => true, "data" => $rows]));
});

route("404", function() {
    response::status(404);
    print(json::encode(["ok" => false, "hata" => "Bulunamadi"]));
});
```

Dosyayı XAMPP `htdocs/` klasörüne at, look-fcgi başlat — bitti.

---

## Kurulum

### Linux (Ubuntu / AlmaLinux) — Tek Komut

```bash
curl -fsSL https://get.look-lang.org | sudo bash -- --with-service --with-nginx --with-cli
```

Kurulum otomatik yapar: distro algılama → binary indir → systemd servis → nginx config → hazır.

### Windows + XAMPP

Binary'leri `C:\xampp\cgi-bin\` altına kopyala, `httpd.conf`'u yapılandır — bkz: `xampp/setup.md`.

### Versiyon doğrulama

```bash
look version       # LOOK 1.0.0 (linux/amd64)
look-fcgi version  # LOOK 1.0.0 (linux/amd64)
```

---

## Binary'ler

| Binary | Amaç |
|--------|------|
| `look-fcgi` | FastCGI — port 9000 (production) |
| `look-fcgi --mode http` | HTTP/1.1 sunucu — Apache bypass, WebSocket, SSE |
| `look-cgi` | CGI — yedek mod |
| `look` | CLI — `look test`, `look repl`, `look install`, `look version` |

---

## Dil Özellikleri (v1.0.0)

| Özellik | Durum |
|---------|-------|
| Tipler: int, float, string, bool, null, array, assoc | ✅ |
| Routing dile gömülü — `route("GET", "/path/{id}", fn)` | ✅ |
| DB: MySQL · SQLite · PostgreSQL — sıfır bağımlılık | ✅ |
| struct (Go stili) + const iota | ✅ |
| switch (Go stili — break yok, çoklu case) | ✅ |
| `parallel()` + `channel()` — Go stili goroutine | ✅ |
| WebSocket — `route("WS", ...)`, `ws::send/broadcast` | ✅ |
| SSE — `route("SSE", ...)` + timer:: after/every/cancel | ✅ |
| Concurrent runtime — `--workers N` | ✅ |
| `--mode http` — epoll (Linux), IOCP (Windows) — sınırsız bağlantı | ✅ |
| Bytecode VM — 7.8x compute speedup | ✅ |
| `use "dosya.lk"` dosya modül sistemi | ✅ |
| Upload: `request::file()`, magic byte, SHA-256 | ✅ |
| template:: engine — layout kalıtımı, partial include | ✅ |
| cache:: — in-memory TTL, thread-safe | ✅ |
| queue:: — named FIFO, cross-request | ✅ |
| jobs:: — SQLite kalıcı kuyruk, retry, delayed, worker | ✅ |
| mail:: — Mailgun / SendGrid / Postmark | ✅ |
| look test runner + look repl + look install | ✅ |
| jobs::recover() — crash recovery | ✅ |

---

## Modüller

```lk
# Core (use gerekmez)
route() · request:: · response:: · db:: · json:: · session:: · cookie:: · log:: · file:: · date::

# Standard
use math;      # sqrt, pow, random, sin, cos...
use string;    # upper, lower, trim, split, replace, slugify...
use array;     # sort, filter, map, reduce, find, any, all...
use type;      # of, is_int, is_string, to_int, to_float...
use auth;      # hash(), verify() — PBKDF2-SHA256
use validator; # required, email, min, max, in
use html;      # escape, strip
use template;  # render($path, $data) — layout, partial, {#if}, {#each}
use http;      # GET/POST/PUT/DELETE — HTTP client, TLS
use cache;     # set/get/has/delete — in-memory TTL, warm start global
use queue;     # push/pop/peek — named FIFO, cross-request
use jobs;      # push/next/done/fail/worker/run — SQLite kalıcı job queue
use mail;      # send/send_html — Mailgun, SendGrid, Postmark
```

---

## parallel() + channel() — Go Stili

```lk
# Fan-out: 3 paralel DB sorgusu
$result = channel(3);

parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM urunler", []));
});
parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM kategoriler", []));
});
parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM firmalar", []));
});

$u = receive($result);
$k = receive($result);
$f = receive($result);
```

---

## WebSocket

```lk
# Sadece --mode http'de çalışır
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
        send($hub, $data);
        ws::broadcast($data);
    });
});
```

---

## mail:: — E-posta

```lk
use mail;

# .env: MAIL_PROVIDER=mailgun, MAIL_API_KEY=..., MAIL_FROM=...
$r = mail::send("user@example.com", "Hoşgeldiniz!", "Kaydınız tamamlandı.");
if (!$r["ok"]) { log::error($r["message"]); }
```

---

## jobs:: — Arka Plan İşleri

```lk
use jobs;

# Route: isteği hemen yanıtla, işi kuyruğa at
route("POST", "/kayit", function() use ($conn) {
    # ... kullanıcı kaydet ...
    jobs::push("email", ["to" => request::post("email")], 3, 0);
    print(json::encode(["ok" => true]));
});

# worker.lk: ayrı process (systemd servis)
use jobs;
use mail;

jobs::recover("email");   # crash recovery — startup'ta

jobs::worker("email", function($job) {
    $r = mail::send($job["payload"]["to"], "Hoşgeldiniz!", "Kaydınız tamamlandı.");
    return $r["ok"];   # false → retry (max 3)
});
jobs::run(5000);   # 5 saniyede bir — blocking
```

---

## Derleme

### Windows

```powershell
cd cpp\build
cmake --build . --config Release
# Çıktı: cpp\build\Release\look-fcgi.exe, look-cgi.exe, look.exe
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

## Performans Referans (looktest.tobiyo.com.tr)

| Mod | Endpoint | RPS | Gecikme |
|-----|----------|-----|---------|
| fcgi (32w) | DB (17 route) | ~7.846 | 27ms |
| http+VM (32w) | /router (DB yok) | **8.276** | 0.12ms |
| http+VM (32w) | /compute (fib40) | **8.685** | 0.12ms |
| http+VM (c=1000) | /router peak | 7.886 | 0.13ms |
| http+VM (32w) | /db (10 row) | ~380 | 2.6ms |

---

## Kurulum Rehberleri

| Platform | Rehber |
|----------|--------|
| Windows + XAMPP | `xampp/setup.md` |
| Ubuntu / AlmaLinux | `docs/ubuntu-deployment.md` |
| Plesk + Apache | `docs/plesk-apache-deployment.md` |
| Linux genel + Plesk hızlı başvuru | `LINUX_PLESK_SETUP.md` |

---

## Desteklenen Platformlar

| Platform | Durum |
|----------|-------|
| Windows 10/11 + XAMPP | ✅ |
| Ubuntu 22.04 / 24.04 + nginx/Apache | ✅ |
| AlmaLinux 8 + Apache | ✅ |
| Plesk (AlmaLinux 8) | ✅ Canlı |

## Desteklenen Veritabanları

| DB | DSN |
|----|-----|
| MySQL / MariaDB | `mysql://user:pass@127.0.0.1/db` |
| SQLite | `sqlite://./data.db` veya `sqlite://:memory:` |
| PostgreSQL | `postgres://user:pass@127.0.0.1:5432/db` |

---

## Canlı Demo

[looktest.tobiyo.com.tr](https://looktest.tobiyo.com.tr) — QR Menu uygulaması, 29 route, MariaDB, Plesk üzerinde çalışıyor.

---

## Dokümantasyon

[docs/index.html](docs/index.html) — Tam dil referansı, API, modüller, örnekler.

---

**v1.0.0-pre** | C++23 | CMake 3.20+ | nginx / Apache 2.4+ | 21 Haziran 2026
