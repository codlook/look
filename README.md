# LOOK Language 
 
NOT: Güvenlik Önlemleri Tam Kapatılmamıştır. 
Projenizi Test Amaçlı Kullanmanız Tavsiye Edilir. 
Son tarih olan 10.07.2026 gününe kadar kararlı sürüm yayınlanmayacaktır.
Look Yazılım Dili Şuan çalışmakta olup güvenlik açıkları Codlook Ekibi tarafından kapatılmaktadır.
Güvenlik açıkları kapatılınca not kısmı otomatik olarak silinmiş olduğunda gönül rahatlılığı ile gerçek prpjelerde kullanabilirsiniz.
10.07.2026 tarihine kadar bekleyin test ürünleri için look dilini kullanabilirsiniz.

LOOK, C++23 ile yazılmış Apache/CGI+FastCGI uyumlu bir web scripting dilidir.
Web için alternatif olarak tasarlanmıştır — routing dile gömülü, framework kurma zorunluluğu yok.

---

## Hızlı Başlangıç

```lk
$conn = db::connect("mysql://root:@127.0.0.1/mydb")
route("GET", "/", function() use ($conn) {
    $rows = db::query($conn, "SELECT * FROM urunler", [])
    print(json::encode(["ok" => true, "data" => $rows]))
})
route("404", function() {
    response::status(404)
    print(json::encode(["ok" => false, "hata" => "Bulunamadi"]))
})
```

Dosyayı XAMPP `htdocs/` klasörüne at, `install.bat` çalıştır — bitti.

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
| crypto:: — SHA-256, HMAC-SHA256, base64url, UUID, random — sıfır bağımlılık | ✅ |
| Modül sistemi — `use "pkg/jwt/jwt.lk"` · `modules/` dizini | ✅ |

---

## Modüller

```lk
# Core (use gerekmez)
route() · request:: · response:: · db:: · json:: · session:: · cookie:: · log:: · file:: · date::

# Standard
use math  # sqrt, pow, random, sin, cos...
use string  # upper, lower, trim, split, replace, slugify...
use array  # sort, filter, map, reduce, find, any, all...
use type  # of, is_int, is_string, to_int, to_float...
use auth  # hash(), verify() — PBKDF2-SHA256
use crypto  # sha256, hmac_sha256, base64url, uuid, random_bytes — JWT ve ödeme için
use validator  # required, email, min, max, in
use html  # escape, strip
use template  # render($path, $data) — layout, partial, {#if}, {#each}
use http  # GET/POST/PUT/DELETE — outbound HTTP client, TLS
use cache  # set/get/has/delete — in-memory TTL, warm start global
use queue  # push/pop/peek — named FIFO, cross-request
use jobs  # push/next/done/fail/worker/run — SQLite kalıcı job queue
use mail  # send/send_html — Mailgun, SendGrid, Postmark
```

---

## parallel() + channel() — Go Stili

```lk
# Fan-out: 3 paralel DB sorgusu
$result = channel(3)
parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM urunler", []))
})
parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM kategoriler", []))
})
parallel(function() use ($result, $conn) {
    send($result, db::col($conn, "SELECT count(*) FROM firmalar", []))
})
$u = receive($result)
$k = receive($result)
$f = receive($result)
```

---

## WebSocket

```lk
# Sadece --mode http'de çalışır
$hub = channel()
route("WS", "/chat", function($ws) use ($hub) {
    parallel(function() use ($ws, $hub) {
        while (true) {
            $msg = receive($hub)
            if ($msg == null) { break }
            ws::send($ws, $msg)
        }
    })
    ws::on($ws, "message", function($data) use ($hub) {
        send($hub, $data)
        ws::broadcast($data)
    })
})
```

---

## mail:: — E-posta

```lk
use mail
# .env: MAIL_PROVIDER=mailgun, MAIL_API_KEY=..., MAIL_FROM=...
$r = mail::send("user@example.com", "Hoşgeldiniz!", "Kaydınız tamamlandı.")
if (!$r["ok"]) { log::error($r["message"]) }
```

---

## crypto:: — Kriptografi

```lk
use crypto
# SHA-256
$hash = crypto::sha256("veri")  # → hex string

# HMAC-SHA256 (webhook imzası, ödeme doğrulama)
$sig = crypto::hmac_sha256($payload, $api_secret)  # → hex string

# Base64 URL-safe (JWT için)
$enc = crypto::base64url_encode("look dili")
$dec = crypto::base64url_decode($enc)
# UUID v4
$id = crypto::uuid()  # → "c60f195d-af1e-49de-..."

# Güvenli rastgele token
$token = crypto::random_string(32)  # → URL-safe base64, 32 bayt entropi

# Timing-safe karşılaştırma (timing attack koruması)
$ok = crypto::constant_compare($given_sig, $expected_sig)
```

## jwt — Resmi JWT Modülü

```lk
use "pkg/jwt/jwt.lk"
# Token üret (1 saat geçerli)
$token = jwt_sign(
    ["user_id" => 42, "rol" => "admin"],
    "gizli-anahtar",
    ["exp" => 3600]
)
# Doğrula — null döner: geçersiz imza veya süresi dolmuş
$payload = jwt_verify($token, "gizli-anahtar")
if ($payload == null) {
    response::status(401)
    print(json::encode(["error" => "Yetkisiz"]))
    return
}
print("Kullanıcı: " . $payload["user_id"])
```

---

## jobs:: — Arka Plan İşleri

```lk
use jobs
# Route: isteği hemen yanıtla, işi kuyruğa at
route("POST", "/kayit", function() use ($conn) {
    # ... kullanıcı kaydet ...
    jobs::push("email", ["to" => request::post("email")], 3, 0)
    print(json::encode(["ok" => true]))
})
# worker.lk: ayrı process (systemd servis)
use jobs
use mail
jobs::recover("email")  # crash recovery — startup'ta

jobs::worker("email", function($job) {
    $r = mail::send($job["payload"]["to"], "Hoşgeldiniz!", "Kaydınız tamamlandı.")
    return $r["ok"]  # false → retry (max 3)
})
jobs::run(5000)  # 5 saniyede bir — blocking
```

---

## Derleme

### Windows

```powershell
cd cpp\build
cmake --build . --config Release
# Çıktı: cpp\build\Release\lk.exe, lk-cgi.exe, lk-fcgi.exe
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
| Windows + XAMPP | `platforms/windows/xampp/` |
| Ubuntu / AlmaLinux | `docs/ubuntu-deployment.md` |
| Plesk + Apache | `docs/plesk-apache-deployment.md` |
| Plesk Extension | `docs/plesk-extension-kurulum.md` |

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
