#!/bin/bash
cp /src/build-linux/look-fcgi /usr/local/bin/look-fcgi
cp /src/build-linux/look-cgi  /usr/local/bin/look-cgi
chmod +x /usr/local/bin/look-fcgi /usr/local/bin/look-cgi

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

cat > /etc/apache2/sites-available/000-default.conf << 'APACHECONF'
<VirtualHost *:80>
    DocumentRoot /var/www/html
    ScriptAlias /cgi-bin/ /usr/local/bin/
    Action look-handler /cgi-bin/look-cgi
    AddHandler look-handler .lk
    <Directory /var/www/html>
        Options +ExecCGI
        AllowOverride None
        Require all granted
        RewriteEngine On
        RewriteRule ^index\.lk$ / [R=301,L]
        RewriteCond %{REQUEST_URI} \.(html|css|js|png|jpg|ico|svg|woff|woff2|ttf)$ [NC]
        RewriteCond %{REQUEST_FILENAME} -f
        RewriteRule "^" - [L]
        RewriteCond %{REQUEST_URI} \.lk$ [NC]
        RewriteCond %{REQUEST_FILENAME} -f
        RewriteRule "^" - [L]
        RewriteRule "^" "fcgi://127.0.0.1:9000/var/www/html/index.lk" [P,QSA,L]
    </Directory>
    <Directory /usr/local/bin>
        Options +ExecCGI
        Require all granted
    </Directory>
</VirtualHost>
APACHECONF

a2dismod mpm_event 2>/dev/null; a2enmod mpm_prefork cgi 2>/dev/null
/usr/local/bin/look-fcgi --port 9000 &
sleep 1
apache2ctl start
echo "=== QR Menu Ubuntu+Apache hazir — http://localhost:8080/ ==="
sleep infinity
