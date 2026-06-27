# LOOK — Ubuntu / AlmaLinux Deployment Rehberi

PHP gibi yükle, Go gibi yaz, framework kurma.

Bu rehber Ubuntu 22.04/24.04 LTS ve AlmaLinux 8/9 üzerinde LOOK uygulamalarını production'a almak için hazırlanmıştır.

---

## Hızlı Kurulum (Önerilen)

```bash
# Tek komutla kur — binary indir, systemd yaz, nginx ayarla
curl -fsSL https://get.look-lang.org | sudo bash -- --with-service --with-nginx --with-cli

# Ya da seçeneklerle:
sudo bash install.sh --with-service --with-nginx --port 9000 --workers 8
```

Kurulum scripti şunları otomatik yapar:
- Distro algılar (Ubuntu → apt, AlmaLinux → dnf)
- `look-fcgi` binary'sini `/usr/local/bin/` altına indirir
- `systemd` servisi yazar, etkinleştirir ve başlatır
- `nginx` `fastcgi_pass` yapılandırması yazar ve reload eder
- Örnek `index.lk` oluşturur

```bash
# Hızlı doğrulama
curl http://localhost/health
look version
```

---

## Manuel Kurulum

### Gereksinimler

- Ubuntu 22.04 LTS / 24.04 LTS **veya** AlmaLinux 8 / Rocky Linux 8+
- nginx veya Apache 2.4+
- MySQL 8.0+ / MariaDB 10.6+ / PostgreSQL 14+ (opsiyonel)
- Root veya sudo erişimi

---

## 1. Apache Kurulumu

```bash
sudo apt update
sudo apt install -y apache2

# Gerekli modüller
# ÖNEMLİ: mpm_prefork + mod_cgi zorunlu — mpm_event + mod_cgid .lk CGI'yi çalıştırmaz
sudo a2dismod mpm_event
sudo a2enmod mpm_prefork cgi rewrite proxy proxy_fcgi actions

sudo systemctl restart apache2
```

---

## 2. LOOK Binary'lerinin Kurulumu

**Seçenek A — install.sh (önerilen):**

```bash
# Ubuntu/AlmaLinux — sadece binary (servis+nginx ayrıca)
sudo bash install.sh --with-cli
```

**Seçenek B — manuel kopyalama:**

```bash
# Geliştirme makinasında derleme (bkz: Linux build rehberi)
scp build-linux/look-fcgi user@sunucu:/usr/local/bin/look-fcgi
scp build-linux/look-cgi  user@sunucu:/usr/local/bin/look-cgi
scp build-linux/look      user@sunucu:/usr/local/bin/look

# Sunucuda izinleri ayarla
sudo chmod +x /usr/local/bin/look-fcgi /usr/local/bin/look-cgi /usr/local/bin/look

# AlmaLinux: /var/www/cgi-bin/ altına da kopyala
sudo cp /usr/local/bin/look-cgi /var/www/cgi-bin/look-cgi
```

**Doğrulama:**

```bash
look version          # LOOK 1.0.0 (linux/amd64)
look-fcgi version     # LOOK 1.0.0 (linux/amd64)
```

---

## 3. Uygulama Dizini

```bash
# Uygulama dosyaları
sudo mkdir -p /var/www/myapp
sudo chown www-data:www-data /var/www/myapp

# index.lk ve statik dosyaları kopyala
scp index.lk user@sunucu:/var/www/myapp/
scp .env      user@sunucu:/var/www/myapp/

# Log dizini
sudo mkdir -p /var/log/look
sudo chown www-data:www-data /var/log/look
```

### .env dosyası (`/var/www/myapp/.env`)

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

## 4. Nginx ile Kullanım (Apache alternatifi)

Apache yerine nginx tercih ediyorsan — `install.sh --with-nginx` otomatik yazar, ya da manuel:

```nginx
# /etc/nginx/conf.d/look.conf
upstream look_backend {
    server 127.0.0.1:9000;
    keepalive 32;
}

server {
    listen 80;
    server_name myapp.com;
    root /var/www/myapp;

    # Statik dosyalar
    location ~* \.(css|js|png|jpg|svg|woff2|ico)$ {
        try_files $uri =404;
        expires 30d;
    }

    # LOOK FastCGI
    location / {
        fastcgi_pass look_backend;
        fastcgi_param SCRIPT_FILENAME $document_root/index.lk;
        fastcgi_param REQUEST_URI     $request_uri;
        fastcgi_param REQUEST_METHOD  $request_method;
        fastcgi_param QUERY_STRING    $query_string;
        fastcgi_param CONTENT_TYPE    $content_type;
        fastcgi_param CONTENT_LENGTH  $content_length;
        fastcgi_param HTTP_COOKIE     $http_cookie;
        fastcgi_param REMOTE_ADDR     $remote_addr;
        fastcgi_read_timeout 30;
    }
}
```

```bash
sudo nginx -t && sudo systemctl reload nginx
```

---

## 5. look-fcgi Servisi (systemd)

`/etc/systemd/system/look-fcgi.service` dosyasını oluştur:

```ini
[Unit]
Description=LOOK FastCGI Server
After=network.target mysql.service
Wants=mysql.service

[Service]
Type=simple
User=www-data
Group=www-data
WorkingDirectory=/var/www/myapp
ExecStart=/usr/local/bin/look-fcgi --port 9000
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Servisi etkinleştir:

```bash
sudo systemctl daemon-reload
sudo systemctl enable look-fcgi
sudo systemctl start look-fcgi

# Kontrol
sudo systemctl status look-fcgi
```

---

## 6. Apache Virtual Host

`/etc/apache2/sites-available/myapp.conf` dosyasını oluştur:

```apache
<VirtualHost *:80>
    ServerName myapp.com
    DocumentRoot /var/www/myapp

    # CGI binary
    ScriptAlias /cgi-bin/ /usr/local/bin/
    Action look-handler /cgi-bin/look-cgi
    AddHandler look-handler .lk

    <Directory /var/www/myapp>
        Options +ExecCGI
        AllowOverride None
        Require all granted

        <IfModule mod_rewrite.c>
            RewriteEngine On

            # index.lk direkt erişimi engelle
            RewriteRule ^index\.lk$ / [R=301,L]

            # Statik dosyalar doğrudan serve edilir
            RewriteCond %{REQUEST_URI} \.(html|htm|css|js|png|jpg|jpeg|gif|ico|svg|woff|woff2|ttf|eot|pdf|txt|map|webp)$ [NC]
            RewriteCond %{REQUEST_FILENAME} -f
            RewriteRule "^" - [L]

            # Dizinler
            RewriteCond %{REQUEST_FILENAME} -d
            RewriteRule "^" - [L]

            # Var olan .lk dosyaları → CGI handler
            RewriteCond %{REQUEST_URI} \.lk$ [NC]
            RewriteCond %{REQUEST_FILENAME} -f
            RewriteRule "^" - [L]

            # Geri kalan her şey → LOOK FastCGI router
            RewriteRule "^" "fcgi://127.0.0.1:9000/var/www/myapp/index.lk" [P,QSA,L]
        </IfModule>
    </Directory>

    ErrorLog ${APACHE_LOG_DIR}/myapp-error.log
    CustomLog ${APACHE_LOG_DIR}/myapp-access.log combined
</VirtualHost>
```

Site'i etkinleştir:

```bash
sudo a2ensite myapp.conf
sudo a2dissite 000-default.conf   # varsayılan siteyi devre dışı bırak
sudo systemctl reload apache2
```

---

## 7. Veritabanı Kurulumu (opsiyonel)

```bash
sudo apt install -y mysql-server

# Güvenlik ayarları
sudo mysql_secure_installation

# Veritabanı ve kullanıcı oluştur
sudo mysql -u root -p <<'SQL'
CREATE DATABASE myapp CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'myapp_user'@'localhost' IDENTIFIED BY 'guclu_sifre';
GRANT ALL PRIVILEGES ON myapp.* TO 'myapp_user'@'localhost';
FLUSH PRIVILEGES;
SQL

# Schema yükle
mysql -u myapp_user -p myapp < schema.sql
```

---

## 8. PostgreSQL Kullanımı

```bash
# Ubuntu
sudo apt install -y postgresql
sudo -u postgres createuser --pwprompt look_user
sudo -u postgres createdb -O look_user lookdb

# AlmaLinux
sudo dnf install -y postgresql-server
sudo postgresql-setup --initdb
sudo systemctl enable --now postgresql
```

```lk
# index.lk — PostgreSQL bağlantısı
$conn = db::connect("postgres://look_user:sifre@127.0.0.1:5432/lookdb");
```

> **Not:** PostgreSQL için `pg_hba.conf` dosyasında `md5` auth olmalı.  
> `host all all 127.0.0.1/32 md5` satırını ekle, sonra `systemctl restart postgresql`.

---

## 9. mail:: E-posta Yapılandırması

```bash
# .env — Mailgun örneği
MAIL_PROVIDER=mailgun
MAIL_API_KEY=key-xxxxxxxxxxxxxxxxxxxxxx
MAIL_FROM=noreply@myapp.com
```

```lk
use mail;

$r = mail::send("user@example.com", "Hoşgeldiniz!", "Kaydınız tamamlandı.");
if (!$r["ok"]) { log::error("Mail hatası: " . $r["message"]); }
```

Desteklenen provider'lar: `mailgun`, `sendgrid`, `postmark`

---

## 10. Doğrulama

```bash
# look-fcgi çalışıyor mu?
sudo systemctl status look-fcgi

# Port dinleniyor mu?
ss -tlnp | grep 9000

# HTTP test
curl -s http://localhost/
curl -s http://localhost/api/test
```

---

## 11. Güncelleme (sıfır kesinti)

```bash
# Yeni binary'yi kopyala
sudo cp look-fcgi-yeni /usr/local/bin/look-fcgi

# index.lk'yı güncelle — look-fcgi mtime farkını görür, hot reload yapar
sudo cp index-yeni.lk /var/www/myapp/index.lk

# Servis restart (yalnızca binary değiştiyse gerekir)
sudo systemctl restart look-fcgi
```

**Hot reload:** `index.lk` değişince look-fcgi mtime farkını otomatik algılar ve setup fazını yeniden çalıştırır. Servis restart gerekmez.

---

## 12. SQLite Kullanımı

MySQL yerine SQLite kullanmak için sadece DSN değiştir:

```lk
# index.lk
$conn = db::connect("sqlite:///var/www/myapp/data.db");
```

SQLite dosyası izinleri:

```bash
sudo chown www-data:www-data /var/www/myapp/data.db
sudo chmod 660 /var/www/myapp/data.db
```

---

## Sorun Giderme

| Sorun | Kontrol |
|-------|---------|
| 502 Bad Gateway | `systemctl status look-fcgi` — servis çalışıyor mu? |
| DB bağlanamıyor | `.env` dosyası `DB_HOST=127.0.0.1` (socket değil TCP) |
| 403 Forbidden | `chown www-data:www-data /var/www/myapp` |
| Log yok | `LOG_DIR` mutlak path olmalı, `www-data` yazma izni olmalı |
| Hot reload çalışmıyor | `index.lk` mtime değişmeli — `touch index.lk` dene |

---

## Windows (XAMPP) ile Karşılaştırma

| | Windows / XAMPP | Ubuntu / Apache |
|--|----------------|-----------------|
| Binary | `look-fcgi.exe` → `C:\xampp\cgi-bin\` | `look-fcgi` → `/usr/local/bin/` |
| Servis | Manuel başlat veya Task Scheduler | `systemctl enable look-fcgi` |
| Config | `httpd.conf` `<Directory>` bloğu | `/etc/apache2/sites-available/myapp.conf` |
| Uygulama | `C:\xampp\htdocs\` | `/var/www/myapp/` |
| Log | `C:\xampp\htdocs\logs\` | `/var/log/look/` |
| DB | XAMPP MySQL | `mysql-server` veya SQLite |

**Felsefe aynı:** binary kopyala, 10 satır config, index.lk'yı koy, bitti.
