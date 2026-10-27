# LOOK Language — Plesk Eklentisi

**Sürüm:** 1.0.0
**Eklenti ID:** `look-lang`
**Hedef:** Plesk Obsidian 18.0+ · systemd Linux (AlmaLinux/RHEL/Rocky/CloudLinux · Ubuntu/Debian Plesk)
**Kaynak:** `platforms/plesk/`
**Canlı örnek:** [test.codlook.com](https://test.codlook.com) — AlmaLinux 8.10, Plesk Obsidian, nginx→Apache→lk-fcgi

LOOK Plesk eklentisi, tek tıkla her Plesk domainini bir **LOOK uygulaması** (kendi
systemd servisi) olarak yayına alır. Tarayıcı-içi kod editörü, canlı per-domain
monitör, log görüntüleyici ve sistem panosuyla tam bir barındırma çalışma alanıdır.
Panel arayüzü **İngilizce**dir (public dağıtım).

---

## Kurulum

Sunucuda **root** olarak (SSH veya Plesk → Tools & Settings → Terminal):

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk.zip "https://github.com/codlook/look/releases/latest/download/look-lang-plesk.zip"
plesk bin extension --install /tmp/look-lang-plesk.zip
```

Beklenen çıktı: `The extension was successfully installed.`

Alternatif — Plesk UI: **Extensions → Upload Extension → `look-lang-plesk.zip`**.

Paneli aç: **Plesk → Extensions → LOOK Language**, veya doğrudan
`https://<sunucu>:8443/modules/look-lang/`.

> **Kurulum notu:** Plesk eklentinin `post-install.php` kancasını kurulumda ve güncellemede
> çalıştırır, ama root değil `psaadm` olarak. Root gerektiren adım (ikili, servis unit'leri)
> `sbin/look-setup`'tadır ve kanca onu Plesk'in root yolu `pm_ApiCli::callSbin` ile çağırır;
> panelin root işlemleri de aynı yoldan `sbin/look-ctl`'e gider (sudo yok, aşağıdaki
> [Root işlemleri](#root-işlemleri-sudo-yok)). Adımı elle tekrar çalıştırmak için:
> `plesk bin extension --exec look-lang post-install.php`.

---

## Çalışma Alanı (Panel)

Sol kenar çubuğu dört görünüme ayrılır:

### Dashboard
Gerçek zamanlı sistem metrikleri — **CPU · Memory · Disk · Uptime** — her biri canlı
mini grafikle (4 saniyede bir örneklenir, uydurma geçmiş yoktur). Altında uygulama
özet tablosu (durum, port, son deploy).

### Applications (Uygulama Yöneticisi)
- **Add New Domain** formu (listenin üstünde): domain seç → script yolu ve boş port
  otomatik dolar → workers + mod → **Add & Start**.
- Uygulama tablosu: domain, canlı durum, port, script, worker/mod.
- **Edit Code** — `index.lk`'i tarayıcıda düzenle → **Save & Redeploy**.
- **Actions ▾** menüsü: Configure · Restart · Stop/Start · **Monitor** · View Logs · Remove.

### Logs
Servis günlükleri (`journalctl -u look-<domain>`), seviye renklendirmeli
(ERR/WARN/OK), **Copy** düğmesiyle panoya kopyalanır.

### Documentation
Panel içinde LOOK hızlı başlangıç, örnek route kodu, mod/servis rehberi.

### Kod Editörü
`index.lk` içeriğini okur → düzenle → **Save & Redeploy** dosyayı yazıp servisi
yeniden başlatır. Yol güvenliği sıkı: yalnızca `domains.json`'daki kayıtlı `.lk`
yolu, `/var/www/vhosts` altında, `..` reddedilir.

### Monitör (per-domain, canlı)
3 saniyede bir gerçek metrik: **durum, CPU%, bellek (RSS), PID, aktif bağlantı,
port, restart sayısı, çalışma süresi** + canlı log kuyruğu. Modaldan doğrudan
yeniden başlat; kapanınca yoklama durur.

---

## Mimari

Her domain, `index.lk` dosyasını çalıştıran bir **systemd servisidir**:
`look-<domain>` (nokta/özel karakterler `-`). Web trafiği:

```
nginx :443  →  Apache :7081 (vhost_ssl.conf ProxyPass)  →  lk-fcgi :<port>
```

- **FastCGI modu** (`fcgi`) — Apache `mod_proxy_fcgi` ile; üretim önerisi.
- **HTTP modu** (`http`) — `lk-fcgi --mode http --bind 127.0.0.1`; Apache yerel porta vekil olur. 1.4.2 öncesi sürümler tüm arayüzlerde dinliyordu (port dışarıdan erişilebilirdi); güncelleme mevcut servisleri de yeniden yazar.

Servis + Apache proxy config'i `enable.sh` oluşturur; `httpdmng --reconfigure-domain`
arka planda uygular (panel bloke olmaz).

Örnek: `example.com` → servis `look-example-com`, port `9100`.

---

## Binary

Eklenti `/opt/look/{lk,lk-fcgi,lk-cgi}` sağlar:
- **RHEL/AlmaLinux:** gömülü RPM `dnf` ile kurulur → `dnf update look-lang` ile
  güncellenir; binary sistem OpenSSL'e dinamik bağlıdır (OS güvenlik yaması korur).
- **Diğer:** gömülü portatif static binary'ye düşer (statik OpenSSL 3.5 LTS + static
  libstdc++, sıfır bağımlılık, tek dosya; sistem CA otomatik tespit).

---

## Durum (State)

Domain listesi panel-yazılabilir Plesk modül var dizininde tutulur:

```
/usr/local/psa/var/modules/look-lang/domains.json
```

> `/opt/look/conf` root'a aittir (ikili klasörü); panel `psaadm` olarak
> çalışır ve oraya yazamaz. Bu yüzden state modül var dizinindedir.

```json
[
  {
    "domain": "look.codlook.com",
    "script": "/var/www/vhosts/codlook.com/look.codlook.com/index.lk",
    "workers": 4,
    "mode": "fcgi",
    "port": 9100,
    "svc": "look-look-codlook-com"
  }
]
```

---

## Root işlemleri (sudo yok)

Panel `psaadm` olarak çalışır ve **sudo kullanmaz**. Servis kurma/durdurma, durum, log ve
monitör gibi root gerektiren her işlem Plesk'in root yolu `pm_ApiCli::callSbin` ile eklentinin
`sbin/look-ctl` betiğine gider. `look-ctl` eylemi, argüman sayısını ve servis adını
(`^look-[a-z0-9-]+$`, çağrı başına tek servis) denetler; başka bir servise ya da dosyaya
dokunamaz.

1.0.33'e kadarki sürümler `/etc/sudoers.d/look-lang` yazıyordu. sudoers'ta `*` boşlukları da
kapsadığından `systemctl enable look-*` kuralı `systemctl enable look-a /tmp/evil.service` ve
`systemctl stop look-a sshd`'yi de izinliyordu. Güncelleme bu dosyayı siler.

---

## Paket Yapısı

```
look-lang-plesk.zip
├── meta.xml                       # Plesk eklenti tanımı
├── post-install / pre-uninstall   # lifecycle hook'ları
├── plib/controllers/IndexController.php   # Plesk Obsidian MVC köprüsü
└── htdocs/
    ├── index.php                  # backend (AJAX action'lar + render)
    ├── phtml/index.phtml          # çalışma alanı arayüzü (İngilizce)
    ├── dist/main.js
    ├── bin/{lk,lk-fcgi,lk-cgi}    # portatif static binary (fallback)
    ├── look-lang.rpm              # RHEL/AlmaLinux birincil kurulum
    └── scripts/
        ├── enable.sh   # domain → systemd servisi + Apache proxy config
        ├── disable.sh  # servisi durdur/kaldır
        ├── status.sh   # servis durumu
        ├── logs.sh     # journalctl (Log görüntüleyici)
        └── monitor.sh  # canlı metrik (Monitör)
```

> **Paketleme notu:** Plesk zip'i PHP `ZipArchive` ile açar ve unix çalıştırma
> bitini korumaz. Bu yüzden zip'in exec izni önemsizdir (script'ler `/bin/bash` ile
> çağrılır); yalnızca **forward-slash yollar + LF satır sonu + `meta.xml` kökte**
> gerekir. Linux'ta `bash platforms/plesk/build.sh` doğru paketi üretir.

---

## Servis Yönetimi

```bash
enable.sh <domain> <script> <workers> <mode> <port>
```

- systemd birimi: `/etc/systemd/system/look-<domain>.service`
- Apache proxy: `/var/www/vhosts/system/<domain>/conf/vhost.conf` + `vhost_ssl.conf`
- Script yoksa örnek `index.lk` oluşturulur
- `httpdmng --reconfigure-domain` arka planda Apache'yi yeniden yükler

---

## Güvenlik Ortam Değişkenleri

Her domain servisine `Environment=` ile verilir (Configure ekranı üzerinden veya
birim dosyasında):

| Değişken | Açıklama |
|---|---|
| `LOOK_FILE_ROOT` | Dosya erişimini bu dizinle sınırlar; path traversal engellenir. **Ayarlanmazsa varsayılan güvenli: çalışma dizinine kısıtlıdır.** `*` ile kısıtsız (opt-out) |
| `LOOK_RATE_LIMIT_RPM` / `_BURST` | IP başına dakika limiti + burst (token bucket) |
| `LOOK_RATE_LIMIT_GLOBAL_RPM` / `_BURST` | Tüm IP toplamı limiti — botnet koruması |
| `LOOK_TRUSTED_PROXY` | Bu IP'den gelen `X-Forwarded-For` gerçek IP kabul edilir |
| `LOOK_SESSION_DRIVER=redis` + `LOOK_REDIS_URL` | Çok sunucu session (RESP2 çekirdekte) |

SMTP relay koruması: port 25 outbound engelli; yalnızca kimlik doğrulamalı 587/465.

---

## Sorun Giderme

**Kurulum "setup failed" hatası verdi** — hata metni nedeni ve durdurulan domain'i adıyla
söyler. Nedeni gider, sonra eklentiyi yeniden kur ya da root olarak:
`plesk bin extension --exec look-lang post-install.php`.

**Domain eklendi ama listede yok** — panel `domains.json`'a yazamıyor. Kontrol:
```bash
ls -l /usr/local/psa/var/modules/look-lang/domains.json   # psaadm:psaadm olmalı
```

**Site 502/403** — servis çalışıyor mu + Apache proxy var mı:
```bash
systemctl status look-<domain>; ss -tlnp | grep :<port>
cat /var/www/vhosts/system/<domain>/conf/vhost_ssl.conf   # ProxyPass görünmeli
```

**Servis "failed"** — genelde `index.lk` yok veya syntax hatası:
```bash
journalctl -u look-<domain> -n 50 --no-pager
```

---

## Sürüm Geçmişi

| Sürüm | Değişiklikler |
|---|---|
| **1.0.0** | Çalışma alanı arayüzü (sidebar: Dashboard/Applications/Logs/Documentation), tarayıcı-içi **kod editörü** (Save & Redeploy), canlı **per-domain monitör** (CPU/RSS/PID/bağlantı/uptime + log kuyruğu), **log görüntüleyici** (journalctl + kopyala), canlı sparkline'lı sistem panosu, **İngilizce** UI. State Plesk modül var dizininde. Script'ler `sudo /bin/bash` ile (exec-bit bağımsız). RPM/dnf birincil + portatif static fallback (statik OpenSSL 3.5 LTS). |
