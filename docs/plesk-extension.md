# LOOK Language — Plesk Eklentisi

**Sürüm:** 1.0.0
**Eklenti ID:** `look-lang`
**Hedef:** Plesk Obsidian 18.0+ · systemd Linux (AlmaLinux/RHEL/Rocky/CloudLinux · Ubuntu/Debian Plesk)
**Kaynak:** `platforms/plesk/`
**Canlı örnek:** [look.codlook.com](https://look.codlook.com) — AlmaLinux 8.10, Plesk Obsidian, nginx→Apache→lk-fcgi

LOOK Plesk eklentisi, tek tıkla her Plesk domainini bir **LOOK uygulaması** (kendi
systemd servisi) olarak yayına alır. Tarayıcı-içi kod editörü, canlı per-domain
monitör, log görüntüleyici ve sistem panosuyla tam bir barındırma çalışma alanıdır.
Panel arayüzü **İngilizce**dir (public dağıtım).

---

## Kurulum

Sunucuda **root** olarak (SSH veya Plesk → Tools & Settings → Terminal):

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk-1.0.0.zip "https://github.com/codlook/look/releases/download/v1.0/look-lang-plesk-1.0.0.zip"
plesk bin extension --install /tmp/look-lang-plesk-1.0.0.zip
```

Beklenen çıktı: `The extension was successfully installed.`

Alternatif — Plesk UI: **Extensions → Upload Extension → `look-lang-plesk-1.0.0.zip`**.

Paneli aç: **Plesk → Extensions → LOOK Language**, veya doğrudan
`https://<sunucu>:8443/modules/look-lang/`.

> **Yeni/temiz sunucu notu:** Plesk, güvenlik gereği eklenti `post-install`
> hook'unu çalıştırmaz ve zip'i açarken dosya çalıştırma bitlerini kaldırır.
> Eklenti bunu, script'leri `sudo /bin/bash <script>` ile çağırarak aşar (çalıştırma
> izni gerekmez). Ancak sudo yetkisi için `/etc/sudoers.d/look-lang` bir kez root
> ile oluşturulmalıdır (aşağıdaki [Sudoers](#sudoers) bölümü). Bu dosya
> kaldır/kur döngülerinde **korunur** — aynı sunucuda tekrar gerekmez.

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
- **HTTP modu** (`http`) — `lk-fcgi --mode http`; doğrudan HTTP portu.

Servis + Apache proxy config'i `enable.sh` oluşturur; `httpdmng --reconfigure-domain`
arka planda uygular (panel bloke olmaz).

Örnek: `look.codlook.com` → servis `look-look-codlook-com`, port `9100`.

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

> `/opt/look/conf` root'a aittir (dnf/RPM binary klasörü); panel `psaadm` olarak
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

## Sudoers

Panel `psaadm` olarak çalışır; servisleri ve script'leri `sudo` ile yönetir.
Plesk exec bitini kaldırdığından script'ler **`/bin/bash` ile** çağrılır:

```
psaadm ALL=(root) NOPASSWD: \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/enable.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/disable.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/status.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/logs.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/monitor.sh *, \
  /bin/systemctl start look-*, /bin/systemctl stop look-*, \
  /bin/systemctl restart look-*, /bin/systemctl enable look-*, \
  /bin/systemctl disable look-*, /bin/systemctl daemon-reload, \
  /usr/local/psa/admin/sbin/websrvmng, /usr/local/psa/admin/bin/httpdmng
```

Yeni sunucuda bir kez root ile:

```bash
S=/usr/local/psa/admin/htdocs/modules/look-lang/scripts
cat > /etc/sudoers.d/look-lang <<SUDO
psaadm ALL=(root) NOPASSWD: /bin/bash $S/enable.sh *, /bin/bash $S/disable.sh *, /bin/bash $S/status.sh *, /bin/bash $S/logs.sh *, /bin/bash $S/monitor.sh *, /bin/systemctl start look-*, /bin/systemctl stop look-*, /bin/systemctl restart look-*, /bin/systemctl enable look-*, /bin/systemctl disable look-*, /bin/systemctl daemon-reload, /usr/local/psa/admin/sbin/websrvmng, /usr/local/psa/admin/bin/httpdmng
SUDO
chmod 0440 /etc/sudoers.d/look-lang
visudo -cf /etc/sudoers.d/look-lang
```

---

## Paket Yapısı

```
look-lang-plesk-1.0.0.zip
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

**`sudo: .../enable.sh: command not found`** — sudoers eksik/yanlış. Yukarıdaki
[Sudoers](#sudoers) bloğunu root ile çalıştır (script'ler `/bin/bash` ile çağrılır,
chmod gerekmez).

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
