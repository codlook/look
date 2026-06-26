# LOOK Paket Sistemi — Mimari Tasarım

> Bu belge v1.0 öncesi alınmış mimari kararlardır. Sonradan değiştirilmez.

---

## 1. look.toml — Paket Tanım Dosyası

### 1a. Kütüphane paketi

```toml
[package]
name    = "stripe"
version = "1.2.0"
author  = "ali"
look    = ">=1.0.0"
entry   = "stripe.lk"
# type = "pure"  ← varsayılan, belirtilmeyebilir
# type = "native" ← C++ extension (v1.1)

[dependencies]
"codlook/http" = "^1.0"
```

### 1b. Uygulama projesi

```toml
[project]
name    = "siparis-servisi"
version = "1.0.0"
entry   = "index.lk"

[dependencies]
"codlook/cache" = "^1.0"
"codlook/mail"  = "^1.0"
"ali/stripe"    = "^3.1"

[dev-dependencies]
"codlook/test"  = "^1.0"
```

**Kural:** `[package]` = kütüphane (yayınlanabilir). `[project]` = uygulama (yayınlanmaz).  
Aynı `look.toml`'da ikisi birden olamaz.

---

## 2. look.lock — Deterministik Kilitleme

```toml
# Otomatik oluşturulur — elle düzenleme
# Git'e commit edilir — CI/CD reproduciblity için
# look install → bu dosyadan tam versiyonları yükler

[[package]]
name     = "codlook/cache"
version  = "1.2.3"
source   = "github.com/codlook/cache"
checksum = "sha256:abc123..."

[[package]]
name     = "codlook/mail"
version  = "1.0.1"
source   = "github.com/codlook/mail"
checksum = "sha256:def456..."

[[package]]
name     = "ali/stripe"
version  = "3.1.0"
source   = "github.com/ali-stripe/look-stripe"
checksum = "sha256:ghi789..."
```

---

## 3. Semver Kural Tablosu

| Belirtim | Anlam | Örnek — izin verilen |
|----------|-------|---------------------|
| `"^1.2.3"` | `>=1.2.3 <2.0.0` | 1.2.3, 1.3.0, 1.9.9 ✅ — 2.0.0 ❌ |
| `"~1.2.3"` | `>=1.2.3 <1.3.0` | 1.2.3, 1.2.9 ✅ — 1.3.0 ❌ |
| `">=1.0.0"` | Tam sürüm alt sınır | 1.0.0, 2.0.0, 3.5.1 ✅ |
| `"1.2.3"` | Tam sürüm pin | sadece 1.2.3 ✅ |
| `"*"` | Herhangi | tüm sürümler (önerilmez) |

---

## 4. Paket Kaynak Türleri

### 4a. GitHub (resmi community yolu)

```toml
"ali/stripe" = "^3.1"
# → github.com/ali/look-stripe veya github.com/ali/stripe
# look CLI önce look- öneki dener, sonra düz isim
```

### 4b. packages.codlook.com (resmi index)

```toml
"codlook/cache" = "^1.0"
# → packages.codlook.com/codlook/cache → GitHub kaynağını işaret eder
# Registry sadece index + checksum, depolama GitHub'da
```

### 4c. Lokal / Vendor

```toml
# look.toml'da yol belirtimi
"mylib" = { path = "./vendor/mylib" }

# Veya doğrudan .lk import (look.toml gerektirmez)
use "vendor/mylib/mylib.lk";
```

### 4d. Git URL (kilitli versiyon)

```toml
"ali/stripe" = { git = "https://github.com/ali/stripe", rev = "a1b2c3d" }
```

---

## 5. CLI Komutları

```bash
look install                    # look.lock'dan tam yükle (CI/prod)
look install ali/stripe         # tek paket ekle + look.toml güncelle
look add codlook/cache          # add alias
look remove ali/stripe          # kaldır
look update                     # look.lock'u semver sınırları içinde güncelle
look update ali/stripe          # tek paket güncelle
look publish                    # packages.codlook.com'a yayınla
look vendor                     # bağımlılıkları vendor/ altına kopyala
look tidy                       # kullanılmayan bağımlılıkları temizle
```

---

## 6. Kurulum Dizin Yapısı

```
~/.look/
  packages/
    codlook/
      cache/
        1.2.3/          ← sürüm altında saklanır
          cache.lk
          look.toml
    ali/
      stripe/
        3.1.0/
          stripe.lk
          helpers.lk
          look.toml
  registry-cache/       ← packages.codlook.com index cache
    codlook_cache.json
  checksums.db          ← doğrulanmış checksum'lar (SQLite)
```

---

## 7. Paket İçinde use Kullanımı

```lk
# stripe.lk — kütüphane entry point
use "codlook/http";          # bağımlılık

function stripe_charge($key, $amount, $currency) {
    $resp = http::post_json("https://api.stripe.com/v1/charges", [...], [
        "Authorization" => "Bearer {$key}"
    ]);
    return $resp;
}
```

```lk
# index.lk — uygulama
use "ali/stripe";            # look.toml'daki bağımlılık

route("POST", "/odeme", function() {
    $resp = stripe_charge(env("STRIPE_KEY"), 1000, "TRY");
    print(json::encode(["ok" => $resp["status"] == 200]));
});
```

---

## 8. Paket Yayınlama — look publish

```toml
# look.toml'da gerekli
[package]
name    = "stripe"
version = "1.2.0"
# ...

[publish]
registry  = "packages.codlook.com"   # varsayılan
license   = "MIT"
tags      = ["payment", "stripe", "api"]
readme    = "README.md"
```

```bash
look publish          # packages.codlook.com'a yükle
look publish --dry    # simüle et, yükleme
```

---

## 9. Güvenlik Modeli

| Kontrol | Mekanizma |
|---------|-----------|
| Checksum doğrulama | SHA-256, look.lock'ta saklı |
| Versiyon pin | look.lock commit'lenir |
| Sandbox | Pure paketler: sadece .lk, C++ yok |
| Native paket | v1.1 — ayrı güvenlik modeli |
| Resmi paketler | `codlook/` prefix → manuel inceleme |
| Yayın imzası | Gelecek (v1.1) |

---

## 10. Native Extension API — v1.1 Tasarım Taslağı

> **v1.0'da implement edilmez.** Bu bölüm API kararlılığı için şimdiden belgelenir.

```cpp
// look_extension.h — v1.1, ABI sabit olacak
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle — VM'e erişim
typedef struct LookVM LookVM;

// LOOK değer tipi (Value'nun C ABI karşılığı)
typedef struct {
    int   type;           // 0=null 1=int 2=float 3=string 4=bool 5=array
    union {
        long long   i;
        double      f;
        const char* s;
        int         b;
        void*       arr;  // LookArray* — look_array_* fonksiyonlarıyla kullan
    } v;
} LookValue;

// Extension lifecycle
void look_extension_init(LookVM* vm);       // modül yüklenince
void look_extension_shutdown(LookVM* vm);   // modül kapanınca

// Fonksiyon tipi — her C++ fonksiyonu bu imzayı taşır
typedef LookValue (*LookExtFn)(LookVM* vm, LookValue* args, int argc);

// Extension fonksiyonu kaydet — init içinde çağrılır
void look_register_fn(LookVM* vm, const char* module, const char* name, LookExtFn fn);

// Value oluşturucular
LookValue look_int(long long v);
LookValue look_float(double v);
LookValue look_string(const char* v);   // kopyalanır
LookValue look_bool(int v);
LookValue look_null(void);

// String helper
const char* look_to_string(LookValue v);   // geçici buffer, kopyala
long long   look_to_int(LookValue v);
double      look_to_float(LookValue v);

#ifdef __cplusplus
}
#endif
```

### Örnek native extension

```cpp
// example_ext.cpp
#include "look_extension.h"
#include <cstring>

static LookValue ext_hello(LookVM* vm, LookValue* args, int argc) {
    const char* name = argc > 0 ? look_to_string(args[0]) : "dünya";
    char buf[256];
    snprintf(buf, sizeof(buf), "Merhaba, %s!", name);
    return look_string(buf);
}

void look_extension_init(LookVM* vm) {
    look_register_fn(vm, "ornek", "merhaba", ext_hello);
}

void look_extension_shutdown(LookVM* vm) {
    // temizlik
}
```

```toml
# look.toml
[package]
name  = "ornek-ext"
type  = "native"
entry = "ornek.lk"   # wrapper .lk
```

```lk
# ornek.lk — native modülü sarmalar
# use "codlook/ornek-ext"; yazıldığında bu çalışır
```

### Derleme

```bash
# look build → .so (Linux) / .dll (Windows)
look build --native
# Çıktı: build/ornek-ext.so
```

---

## Karar Geçmişi

| Tarih | Karar | Gerekçe |
|-------|-------|---------|
| 20 Haz 2026 | look.toml formatı onaylandı | Go modules mantığı, semver |
| 20 Haz 2026 | Registry = GitHub tabanlı | v1.0'da infra gerekmez |
| 20 Haz 2026 | Native extension v1.1'e ertelendi | ABI sabit olmadan dağıtılamaz |
| 20 Haz 2026 | `codlook/` resmi prefix | npm @scope mantığı |
| 20 Haz 2026 | look.lock Git'e commit edilir | Reproducible builds |
