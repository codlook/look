# LOOK — Plesk + Apache Deployment Rehberi

PHP gibi yukle, Go gibi yaz, framework kurma.

> **Not:** Plesk Extension kullaniyorsan bu rehbere gerek yok.  
> Bak: [plesk-extension-kurulum.md](plesk-extension-kurulum.md) — ZIP yukle, 1 komut, domain ekle, bitti.  
> Bu rehber **manuel kurulum** icin (Plesk Extension kullanmayanlar veya SSH tercih edenler).

Bu rehber **VPS uzerinde Plesk** kullanan sunucularda LOOK uygulamalarini Apache ile production'a almak icin hazirlanmistir.

**Gereksinimler:**
- VPS veya Dedicated sunucu (root veya sudo erişimi zorunlu)
- Plesk Obsidian veya üzeri
- Apache web server (Plesk kurulumunda seçili)
- Ubuntu 22.04/24.04 LTS veya AlmaLinux 8 (Plesk'in önerdiği dağıtımlar)

**Platform notu:**
| Platform | Binary | Fark |
|----------|--------|------|
| Ubuntu + Plesk | `build-linux/look-fcgi` | `a2enmod` kullanılır |
| AlmaLinux 8 + Plesk | `build-almalinux8/look-fcgi` | `httpd`, `/var/www/cgi-bin/` |

---

## 1. LOOK Binary Kurulumu

Sunucuya SSH ile bağlan:

```bash
ssh root@sunucu_ip
```

Binary'leri kopyala:

```bash
# Ubuntu + Plesk için
scp cpp/build-linux/look-fcgi root@sunucu_ip:/opt/look/look-fcgi
scp cpp/build-linux/look-cgi  root@sunucu_ip:/opt/look/look-cgi

# AlmaLinux 8 + Plesk için (glibc uyumu — Ubuntu binary'si çalışmaz)
scp cpp/build-almalinux8/look-fcgi root@sunucu_ip:/opt/look/look-fcgi
scp cpp/build-almalinux8/look-cgi  root@sunucu_ip:/opt/look/look-cgi

# İzinleri ayarla (pscp execute bit'i kaybeder — chmod zorunlu!)
chmod +x /opt/look/look-fcgi /opt/look/look-cgi
```

> **Kritik:** `scp` / `pscp` ile yüklenen binary'lerde execute bit kaybolur. Upload sonrası mutlaka `chmod +x` çalıştır. Binary yüklendikten sonra `systemctl restart` da gerekir.

---

## 2. Uygulama Dizini

Plesk'te her domain `/var/www/vhosts/domain.com/httpdocs/` altında yaşar:

```bash
# Uygulama dizini (Plesk otomatik oluşturur)
APPDIR=/var/www/vhosts/domain.com/httpdocs

# index.lk ve dosyaları kopyala
scp index.lk root@sunucu_ip:$APPDIR/
scp .env      root@sunucu_ip:$APPDIR/

# Log dizini
mkdir -p /var/log/look
chown psacln:psacln /var/log/look   # Plesk kullanıcısı
```

### .env (`/var/www/vhosts/domain.com/httpdocs/.env`)

```
APP_ENV=production
DB_HOST=127.0.0.1
DB_PORT=3306
DB_NAME=myapp
DB_USER=myapp_user
DB_PASS=guclu_sifre
LOG_DIR=/var/log/look
```

---

## 3. look-fcgi Servisi (systemd)

Her domain için ayrı port kullanılır. Birden fazla LOOK uygulaması varsa port numaraları farklı olmalı (9000, 9001, 9002...).

`/etc/systemd/system/look-fcgi-domain.service` oluştur:

```ini
[Unit]
Description=LOOK FastCGI — domain.com
After=network.target mariadb.service

[Service]
Type=simple
ExecStart=/opt/look/look-fcgi --port 9000
WorkingDirectory=/var/www/vhosts/domain.com/httpdocs
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

> **Not:** `User=psacln` eklersen binary `/opt/look/` için okuma izni gerekir. Root olarak çalıştırmak Plesk VPS'lerde yaygın ve pratiktir.

Servisi başlat:

```bash
systemctl daemon-reload
systemctl enable look-fcgi-domain
systemctl start look-fcgi-domain

# Kontrol
systemctl status look-fcgi-domain
```

---

## 4. Apache Konfigürasyonu — .htaccess (Önerilen)

Plesk'te `vhost_ssl.conf` otomatik include edilmez. En güvenilir yöntem: kuralları doğrudan `.htaccess`'e yazmak.

`/var/www/vhosts/domain.com/httpdocs/.htaccess`:

```apache
RewriteEngine On

# index.lk direkt erişimi engelle
RewriteRule ^index\.lk$ / [R=301,L]

# Statik dosyalar doğrudan serve edilir
RewriteCond %{REQUEST_URI} \.(html|htm|css|js|png|jpg|jpeg|gif|ico|svg|woff|woff2|ttf|eot|pdf|txt|map|webp)$ [NC]
RewriteCond %{REQUEST_FILENAME} -f
RewriteRule ^ - [L]

# Dizinler
RewriteCond %{REQUEST_FILENAME} -d
RewriteRule ^ - [L]

# Var olan .lk dosyaları → look-fcgi (direct mode — route'suz scriptler)
RewriteCond %{REQUEST_URI} \.lk$ [NC]
RewriteCond %{REQUEST_FILENAME} -f
RewriteRule ^(.+\.lk)$ fcgi://127.0.0.1:9000/$1 [P,QSA,L]

# Geri kalan her şey → LOOK FastCGI router
RewriteRule ^ fcgi://127.0.0.1:9000/index.lk [P,QSA,L]
```

**Neden `Action look-handler` değil?**  
Plesk'te `ScriptAlias /cgi-bin/` tanımlı değil, `Action` direktifi için gerekli URL path çalışmıyor. `.lk` dosyalarını doğrudan look-fcgi'ye yönlendirmek daha temiz: look-fcgi `route()` içermeyen scriptleri otomatik olarak "direct mode"da çalıştırır (her request'te fresh interpret).

**Alternatif — Plesk Panel (Additional Directives):**  
Plesk Panel → Domains → domain.com → Apache & Nginx Settings → "Additional directives for HTTPS" alanına da aynı RewriteEngine kuralları yazılabilir. Ancak `.htaccess` daha hızlı güncellenir ve restart gerektirmez.

**Kritik `.htaccess` uyarıları:**
- `Options +FollowSymLinks` yazma — Plesk AllowOverride bunu engelliyor, 500 döner
- `Options +ExecCGI` yazabilirsin (AllowOverride izin veriyor) ama CGI yolu için gerekmiyor
- `vhost_ssl.conf` oluşturup içine kural yazmak işe yaramıyor — Plesk otomatik include etmiyor

---

## 5. Apache Modülleri Kontrolü

Plesk genellikle gerekli modülleri kurar ama kontrol et:

```bash
# Ubuntu + Plesk
apachectl -M | grep -E 'proxy_fcgi|rewrite|actions|cgi'
# Eksikse:
a2dismod mpm_event
a2enmod mpm_prefork cgi proxy proxy_fcgi rewrite actions
systemctl restart apache2

# AlmaLinux 8 + Plesk (httpd)
apachectl -M | grep -E 'proxy_fcgi|rewrite|actions|cgi'
# httpd'de modüller /etc/httpd/conf.modules.d/ altında yönetilir
# Plesk kurulumunda proxy_fcgi genellikle zaten aktiftir
```

> **Not:** Ubuntu Apache'de `.lk` CGI action çalışması için `mpm_prefork` zorunludur. `mpm_event` + `mod_cgid` kombinasyonu LOOK CGI binary'sini çalıştırmaz.

---

## 6. Doğrulama

```bash
# look-fcgi çalışıyor mu?
systemctl status look-fcgi-domain

# Port dinleniyor mu?
ss -tlnp | grep 9000

# HTTP test
curl -s http://domain.com/
curl -s http://domain.com/api/test
```

---

## 7. Birden Fazla LOOK Uygulaması

Her domain için ayrı servis ve port:

| Domain | Servis dosyası | Port |
|--------|---------------|------|
| domain.com | look-fcgi-domain.service | 9000 |
| shop.com | look-fcgi-shop.service | 9001 |
| blog.com | look-fcgi-blog.service | 9002 |

Her domain'in **Additional Directives** alanında `fcgi://127.0.0.1:PORT` kısmını ilgili port ile güncelle.

---

## 8. Güncelleme (sıfır kesinti)

```bash
# index.lk güncelle — look-fcgi mtime farkını görür, hot reload yapar
# Servis restart gerekmez
scp index-yeni.lk root@sunucu_ip:/var/www/vhosts/domain.com/httpdocs/index.lk

# Binary güncellendiyse servis restart gerekir
scp build-linux/look-fcgi root@sunucu_ip:/usr/local/bin/look-fcgi
systemctl restart look-fcgi-domain
```

---

## Sorun Giderme

| Sorun | Kontrol |
|-------|---------|
| 502 Bad Gateway | `systemctl status look-fcgi` — servis çalışıyor mu? `ss -tlnp \| grep 9000` — port dinleniyor mu? |
| 500 Internal Server Error | `.htaccess`'te `Options +FollowSymLinks` varsa sil — Plesk AllowOverride bunu engelliyor |
| `hello.lk` → 404 "Endpoint bulunamadi" | look-fcgi eski sürüm (direct mode yok). Binary'yi yeniden derle ve `chmod +x` yap |
| Session çalışmıyor / 401 | `session::start()` global scope'ta çağrılıyor. Her session kullanan route callback içine taşı |
| Login cookie gelmiyor | `session::start()` route içinde mi? `Set-Cookie` header var mı? (`curl -v`) |
| Binary çalışmıyor (203/EXEC) | `chmod +x /opt/look/look-fcgi` — pscp execute bit'i siliyor |
| DB bağlanamıyor | `.env` → `DB_HOST=127.0.0.1` (socket değil TCP). `systemctl restart look-fcgi` sonrası hot reload gerekir |
| Rewrite çalışmıyor | `apachectl -M \| grep rewrite` — mod_rewrite aktif mi? `.htaccess` syntax hatası için Apache error log'a bak |
| Log yok | `LOG_DIR` mutlak path (`/var/log/look/`), dizin yazma izni var mı? |
| nginx 404 (static) | nginx tüm istekleri Apache'ye proxylıyor — 404 Apache'den geliyor, nginx'ten değil. Apache log'a bak |

---

## XAMPP / Ubuntu / Plesk Karşılaştırması

| | XAMPP (Windows) | Ubuntu + Apache | Plesk + Apache |
|--|----------------|-----------------|----------------|
| Binary | `cgi-bin/look-fcgi.exe` | `/usr/local/bin/look-fcgi` | `/usr/local/bin/look-fcgi` |
| Servis | Manuel / Task Scheduler | `systemctl` | `systemctl` |
| Config | `httpd.conf` `<Directory>` | vhost `.conf` dosyası | Additional Directives (UI) |
| Uygulama | `htdocs/` | `/var/www/myapp/` | `/var/www/vhosts/domain.com/httpdocs/` |

**Felsefe değişmedi:** binary kopyala, birkaç satır config, index.lk at, bitti.

---

## Gerçek Kurulum — looktest.tobiyo.com.tr

| Alan | Değer |
|------|-------|
| Sunucu | 193.111.125.66 |
| OS | AlmaLinux 8.10 |
| Panel | Plesk Obsidian |
| Domain | looktest.tobiyo.com.tr |
| Binary | `/opt/look/look-fcgi` |
| Servis | `look-fcgi.service` (port 9000) |
| Uygulama | `/var/www/vhosts/tobiyo.com.tr/looktest.tobiyo.com.tr/` |
| DB | MariaDB 10.3.39 — looktestdb |
| Mimari | nginx:443 → Apache:7081 → mod_proxy_fcgi → look-fcgi:9000 |
