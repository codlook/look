#!/bin/bash
set -e

echo "=== LOOK Ubuntu + Apache + MySQL Tam Stack Test ==="

# Binary ve app dosyalarını kopyala
cp /src/build-linux/look-fcgi /usr/local/bin/look-fcgi
cp /src/build-linux/look-cgi  /usr/local/bin/look-cgi
chmod +x /usr/local/bin/look-fcgi /usr/local/bin/look-cgi

# Uygulama dizini — Apache default sayfasını kaldır
mkdir -p /var/www/html
rm -f /var/www/html/index.html
cp /src/test_real_app.lk /var/www/html/index.lk

# Apache virtual host config
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
        RewriteCond %{REQUEST_URI} \.(html|htm|css|js|png|jpg|jpeg|gif|ico|svg)$ [NC]
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

    ErrorLog /var/log/apache2/look-error.log
    CustomLog /var/log/apache2/look-access.log combined
</VirtualHost>
APACHECONF

# mpm_prefork + mod_cgi gerekli (mpm_event + mod_cgid çalışmaz)
a2dismod mpm_event 2>/dev/null; a2enmod mpm_prefork cgi 2>/dev/null

# look-fcgi başlat
echo "--- look-fcgi başlatılıyor ---"
/usr/local/bin/look-fcgi --port 9000 &
FCGI_PID=$!
sleep 2

# Apache başlat
echo "--- Apache başlatılıyor ---"
apache2ctl start
sleep 1

echo ""
echo "=== TEST 1: GET /saglik ==="
curl -s http://localhost/saglik

echo ""
echo "=== TEST 2: GET / (boş liste) ==="
curl -s http://localhost/

echo ""
echo "=== TEST 3: POST /not (yeni not ekle) ==="
RESP=$(curl -s -X POST http://localhost/not \
     -H "Content-Type: application/json" \
     -d '{"baslik":"Merhaba LOOK","icerik":"Ubuntu Apache test"}')
echo $RESP
ID=$(echo $RESP | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])" 2>/dev/null)

echo ""
echo "=== TEST 4: POST /not (ikinci not) ==="
curl -s -X POST http://localhost/not \
     -H "Content-Type: application/json" \
     -d '{"baslik":"AlmaLinux siradaki","icerik":"Plesk deployment hazir"}'

echo ""
echo "=== TEST 5: GET / (dolu liste) ==="
curl -s http://localhost/

echo ""
echo "=== TEST 6: GET /not/$ID ==="
curl -s http://localhost/not/$ID

echo ""
echo "=== TEST 7: DELETE /not/$ID ==="
curl -s -X DELETE http://localhost/not/$ID

echo ""
echo "=== TEST 8: GET /not/$ID (silinmiş) ==="
curl -s http://localhost/not/$ID

echo ""
echo "=== TEST 9: 404 testi ==="
curl -s http://localhost/olmayan/endpoint

echo ""
echo ""
echo "=== TÜM TESTLER TAMAMLANDI ==="

kill $FCGI_PID 2>/dev/null
apache2ctl stop 2>/dev/null
