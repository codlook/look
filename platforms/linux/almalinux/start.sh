#!/bin/bash
set -e

cp /src/build-almalinux8/look-fcgi /usr/local/bin/look-fcgi
cp /src/build-almalinux8/look-cgi  /usr/local/bin/look-cgi
chmod +x /usr/local/bin/look-fcgi /usr/local/bin/look-cgi

# AlmaLinux 8 httpd default ScriptAlias /var/www/cgi-bin/ kullanır
mkdir -p /var/www/cgi-bin
cp /usr/local/bin/look-cgi /var/www/cgi-bin/look-cgi
chmod +x /var/www/cgi-bin/look-cgi

mkdir -p /var/www/html
rm -f /var/www/html/index.html
cp /xampp/index.lk /var/www/html/index.lk
cp /xampp/demo.lk  /var/www/html/demo.lk

cat > /var/www/html/.env << 'ENVEOF'
APP_ENV=production
DB_HOST=mysql
DB_PORT=3306
DB_NAME=qrmenu
DB_USER=root
DB_PASS=testpass
LOG_DIR=/var/log/look
ENVEOF

mkdir -p /var/log/look
chown apache:apache /var/log/look 2>/dev/null || true

# AlmaLinux 8 httpd — mpm_prefork varsayılan, mod_cgi zaten aktif
# mod_proxy_fcgi ve mod_actions'ı etkinleştir
cat > /etc/httpd/conf.d/look.conf << 'APACHECONF'
LoadModule proxy_module modules/mod_proxy.so
LoadModule proxy_fcgi_module modules/mod_proxy_fcgi.so
LoadModule actions_module modules/mod_actions.so

ScriptAlias /cgi-bin/ /usr/local/bin/
Action look-handler /cgi-bin/look-cgi
AddHandler look-handler .lk

<Directory /usr/local/bin>
    Options +ExecCGI
    Require all granted
</Directory>

<VirtualHost *:80>
    DocumentRoot /var/www/html

    <Directory /var/www/html>
        Options +ExecCGI
        AllowOverride None
        Require all granted

        RewriteEngine On
        RewriteRule ^index\.lk$ / [R=301,L]
        RewriteCond %{REQUEST_URI} \.(html|htm|css|js|png|jpg|jpeg|gif|ico|svg|woff|woff2|ttf|eot|pdf|txt|map|webp)$ [NC]
        RewriteCond %{REQUEST_FILENAME} -f
        RewriteRule "^" - [L]
        RewriteCond %{REQUEST_URI} \.lk$ [NC]
        RewriteCond %{REQUEST_FILENAME} -f
        RewriteRule "^" - [L]
        RewriteRule "^" "fcgi://127.0.0.1:9000/var/www/html/index.lk" [P,QSA,L]
    </Directory>

    ErrorLog /var/log/httpd/look-error.log
    CustomLog /var/log/httpd/look-access.log combined
</VirtualHost>
APACHECONF

# mod_rewrite etkin mi kontrol et
grep -q "^LoadModule rewrite_module" /etc/httpd/conf.modules.d/*.conf 2>/dev/null || \
    echo "LoadModule rewrite_module modules/mod_rewrite.so" >> /etc/httpd/conf.d/look.conf

# look-fcgi başlat
echo "--- look-fcgi başlatılıyor ---"
/usr/local/bin/look-fcgi --port 9000 &
sleep 2

# httpd başlat
echo "--- httpd başlatılıyor ---"
httpd -k start
sleep 1

echo "=== AlmaLinux 8 + Apache (httpd) hazır ==="
echo "=== http://localhost:8081/ ==="
sleep infinity
