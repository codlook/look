# patch_httpd.ps1 — LOOK FastCGI icin httpd.conf yamalar
# Kullanim: .\patch_httpd.ps1 [-ConfPath "C:\xampp\apache\conf\httpd.conf"] [-Port 9000]
param(
    [string]$ConfPath = "C:\xampp\apache\conf\httpd.conf",
    [string]$Port = "9000"
)

if (-not (Test-Path $ConfPath)) {
    Write-Error "httpd.conf bulunamadi: $ConfPath"
    exit 1
}

$content = Get-Content $ConfPath -Raw -Encoding UTF8

# Zaten yamali mi?
if ($content -match 'lk-fcgi\.exe') {
    Write-Host "[OK]  httpd.conf zaten yamali."
    exit 0
}

# Yedek al
$backup = $ConfPath + ".bak"
Copy-Item $ConfPath $backup -Force
Write-Host "[OK]  Yedek: $backup"

# mod_proxy ve mod_proxy_fcgi modullerini etkinlestir
foreach ($mod in @('proxy_module', 'proxy_fcgi_module', 'rewrite_module')) {
    if ($content -match "(?m)^#(LoadModule\s+$mod\s+\S+)") {
        $content = $content -replace "(?m)^#(LoadModule\s+$mod\s+\S+)", '$1'
        Write-Host "[OK]  LoadModule $mod etkinlestirildi."
    } else {
        Write-Host "[INFO] LoadModule $mod zaten aktif."
    }
}

# <Directory "C:/xampp/htdocs"> blogu icine eklenecek konfigurasyon
$lookBlock = @"

    # --- LOOK Language FastCGI (lk-fcgi.exe --port $Port) ---
    <FilesMatch "\.lk$">
        SetHandler "proxy:fcgi://127.0.0.1:${Port}"
    </FilesMatch>

    <IfModule mod_rewrite.c>
        RewriteEngine On
        # Statik dosyalari dogrudan sun
        RewriteCond %{REQUEST_FILENAME} -f [OR]
        RewriteCond %{REQUEST_FILENAME} -d
        RewriteRule ^ - [L]
        # Diger tum istekleri index.lk'ya yonlendir
        RewriteRule ^ /index.lk [L]
    </IfModule>
    # --- /LOOK Language ---
"@

$pattern = '(<Directory\s+"C:/xampp/htdocs">[\s\S]*?)(</Directory>)'
if ($content -match $pattern) {
    $newContent = $content -replace $pattern, "`$1$lookBlock`n`$2"
    [System.IO.File]::WriteAllText($ConfPath, $newContent, [System.Text.Encoding]::UTF8)
    Write-Host "[OK]  httpd.conf yamalandi."
    Write-Host ""
    Write-Host "Simdi su adimlari izle:"
    Write-Host "  1. lk-fcgi.exe'yi C:\look\ klasorune kopyala"
    Write-Host "  2. Powershell'de: Start-Process C:\look\lk-fcgi.exe -ArgumentList '--port $Port' -WindowStyle Hidden"
    Write-Host "  3. XAMPP Apache'yi yeniden baslt"
    exit 0
} else {
    Write-Warning "Directory blogu bulunamadi -- su blogu manuel olarak httpd.conf icine ekleyin:"
    Write-Host $lookBlock
    exit 1
}
