# LOOK Language — Plesk Eklentisi

**Sürüm:** 1.1.0  
**Hedef Sunucu:** AlmaLinux 8, Plesk Obsidian 18.0.76+  
**Eklenti ID:** `look-lang`

---

## Genel Bakış

Bu eklenti, LOOK dil yorumlayıcısını (`lk`, `lk-cgi`, `lk-fcgi`) Plesk paneline entegre eder.  
Sunucudaki Plesk domainleri için LOOK FastCGI veya HTTP Proxy servisi oluşturur, yönetir ve izler.

---

## Dosya Yapısı

```
extensions/plesk/
├── meta.xml                        # Plesk eklenti tanımı
├── scripts/
│   └── post-install                # Plesk lifecycle hook
└── htdocs/                         # Eklenti web arayüzü (Plesk'e kopyalanır)
    ├── index.php                   # Yönetim paneli
    ├── icon.png                    # Eklenti ikonu
    └── scripts/
        ├── install.sh              # Binary kur, sudoers ayarla, PATH ekle
        ├── enable.sh               # Domain için systemd servisi + nginx config oluştur
        ├── disable.sh              # Servisi durdur (domain listeden silinmez)
        ├── status.sh               # Servis durumu sorgula
        └── uninstall.sh            # Temizlik
```

---

## Kurulum

### 1. Binary Derleme (AlmaLinux 8)

AlmaLinux 8'in sistem GCC'si C++23'ü desteklemez. `gcc-toolset-11` gereklidir:

```bash
dnf install -y epel-release
dnf install -y gcc-toolset-11 gcc-toolset-11-gcc-c++ cmake make
source /opt/rh/gcc-toolset-11/enable
cmake -B build -DCMAKE_CXX_COMPILER=/opt/rh/gcc-toolset-11/root/usr/bin/g++ ...
cmake --build build --target lk --target lk-cgi --target lk-fcgi
```

Derlenmiş binary'ler `extensions/plesk/htdocs/bin/` klasörüne konur:
- `lk` (~3.5 MB)
- `lk-cgi` (~3.6 MB)
- `lk-fcgi` (~4.0 MB)

### 2. Plesk'e Yükleme

```bash
# ZIP oluştur
cd extensions/plesk
zip -r look-lang.zip htdocs/ meta.xml scripts/

# Plesk'e yükle
plesk bin extension -i look-lang.zip

# install.sh manuel çalıştır (Plesk lifecycle hook çalışmıyor olabilir)
bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/install.sh
```

### 3. install.sh Ne Yapar?

- Binary'leri `/opt/look/` klasörüne kopyalar (`lk`, `lk-cgi`, `lk-fcgi`)
- Geriye dönük uyumluluk için symlink oluşturur: `look → lk`, `look-fcgi → lk-fcgi`, vb.
- `/etc/sudoers.d/look-lang` dosyası oluşturur (psaadm yetkisi)
- `/opt/look/conf/` dizinini psaadm'e verir
- `domains.json` dosyasını psaadm yazabilir yapar
- `/var/log/look/` log dizinini oluşturur
- `/opt/look/` PATH'e eklenir (`/etc/profile.d/look.sh`)
- Logrotate kuralı eklenir

---

## Panel Özellikleri (`index.php`)

### Üst Bar — Sistem İstatistikleri
- CPU kullanımı (% + core sayısı)
- RAM (kullanılan / toplam MB)
- Disk (kullanılan / toplam, doluluk %)
- Load average (1m 5m 15m)
- Uptime
- İşletim sistemi

### Aktif LOOK Domainleri

Her domain için kart görünümü:
- Domain adı, script yolu, port, worker sayısı, mod, servis adı
- Durum rozeti: **active** (yeşil) / **inactive** (sarı) / **unknown** (gri)
- Active iken PID numarası gösterilir

**Butonlar (duruma göre değişir):**

| Durum | Düzenle | Başlat/Yeniden Başlat | Durdur | Sil |
|---|---|---|---|---|
| active | ✓ | Yeniden Başlat (sarı) | Durdur (kırmızı, aktif) | ✓ |
| inactive | ✓ | Başlat (yeşil) | Durdur (kırmızı, devre dışı) | ✓ |

- **Başlat / Yeniden Başlat** → `enable.sh` çağırır (servis dosyası silinmiş olsa bile yeniden oluşturur)
- **Durdur** → yalnızca servisi durdurur, domain listeden **silinmez**
- **Sil** → servisi durdurur ve listeden kaldırır (onay sorar)
- **Düzenle** → aynı kartın altında inline form açar (script yolu, port, workers, mod)

### Domain Ekle Formu

- Plesk'teki tüm domainler dropdown'da listelenir (`plesk bin site --list`)
- Domain seçince script yolu ve port otomatik dolar
- Port numarası sistem portlarıyla çakışmayan ilk boş portu otomatik seçer (9000'den başlar)
- **Başlat** butonu → enable.sh çalıştırır, 2 saniye bekler, domains.json günceller

### Son İşlem Logu

Son çalıştırılan script çıktısı (`/tmp/look_last.log`) panelde gösterilir.

---

## Servis Yönetimi

### enable.sh

```
enable.sh <domain> <script> <workers> <mode> <port>
```

- systemd servis dosyası: `/etc/systemd/system/look-<domain-sanitized>.service`
- nginx vhost config: `/var/www/vhosts/<domain>/conf/vhost_nginx.conf`
- Script dosyası yoksa otomatik `index.lk` oluşturur
- Plesk domain reload: `plesk sbin httpdmng --reconfigure-domain`

Servis adı formatı: `look-` + domain (küçük harf, nokta ve özel karakterler `-` ile değiştirilir)  
Örnek: `looktest.tobiyo.com.tr` → `look-looktest-tobiyo-com-tr`

### disable.sh

```
disable.sh <domain> [svc_name]
```

- Servisi durdurur ve devre dışı bırakır
- İkinci parametre verilmezse servis adını domain'den otomatik hesaplar

### FastCGI Modu (fcgi)

nginx, LOOK'u FastCGI olarak çağırır. Tüm istekler `index.lk` üzerinden yönlendirilir:

```nginx
location / {
    try_files $uri $uri/ @look;
}
location @look {
    include fastcgi_params;
    fastcgi_pass 127.0.0.1:<port>;
    fastcgi_param SCRIPT_FILENAME <script>;
    ...
}
```

### HTTP Proxy Modu (http)

LOOK HTTP sunucusu olarak çalışır, nginx reverse proxy yapar:

```nginx
location / {
    proxy_pass http://127.0.0.1:<port>;
}
```

---

## Durum Yönetimi (`domains.json`)

`/opt/look/conf/domains.json` — Panel tarafından yönetilen domain listesi:

```json
[
  {
    "domain": "looktest.tobiyo.com.tr",
    "script": "/var/www/vhosts/looktest.tobiyo.com.tr/httpdocs/index.lk",
    "workers": 32,
    "mode": "fcgi",
    "port": 9000,
    "svc": "look-looktest-tobiyo-com-tr"
  }
]
```

**Önemli:** Dosya sahibi `psaadm` olmalı, yoksa panel yazamaz:
```bash
chown psaadm:psaadm /opt/look/conf/domains.json
chmod 664 /opt/look/conf/domains.json
```

---

## Sudoers Yapılandırması

`/etc/sudoers.d/look-lang`:

```
psaadm ALL=(root) NOPASSWD: \
  /usr/local/psa/admin/htdocs/modules/look-lang/scripts/enable.sh, \
  /usr/local/psa/admin/htdocs/modules/look-lang/scripts/disable.sh, \
  /usr/local/psa/admin/htdocs/modules/look-lang/scripts/status.sh, \
  /bin/systemctl start look-*, \
  /bin/systemctl stop look-*, \
  /bin/systemctl restart look-*, \
  /bin/systemctl daemon-reload
```

> **Not:** sudo komutlarında tam yol (`/bin/systemctl`) zorunludur — sudoers'ta bu şekilde kayıtlıdır.

---

## Geriye Dönük Uyumluluk

`lk` binary'leri `look` adıyla çalışan eski servislerle uyumlu olması için symlink oluşturulur:

```bash
/opt/look/look     → /opt/look/lk
/opt/look/look-cgi → /opt/look/lk-cgi
/opt/look/look-fcgi → /opt/look/lk-fcgi
```

Eski servis adlarıyla (örn. `look-fcgi`) çalışan mevcut servisler yeni binary'yi otomatik kullanır.  
Panel, `domains.json`'daki `svc` alanıyla doğru servisi takip eder.

---

## Plesk Panel URL

```
https://<plesk-ip>/modules/look-lang/
```

---

## Bilinen Notlar

- **PHP uyumluluğu:** Panel PHP 5.4+ uyumlu yazılmıştır (tip tanımları yok, `??` operatörü yok, `define()` kullanılmıyor). Plesk'in iç PHP'si CLI sürümünden daha eski olabilir.
- **PRG pattern:** Form POST işlemleri redirect ile biter — nginx/Apache reload PHP bağlantısını kesemez.
- **Arka plan çalıştırma:** `enable.sh` ve `disable.sh` `& (background)` ile çalıştırılır, sonuç `/tmp/look_last.log`'a yazılır.
- **Port çakışması:** `next_free_port()` hem `domains.json`'daki portları hem de `ss -tlnp` ile sistem portlarını kontrol eder.
