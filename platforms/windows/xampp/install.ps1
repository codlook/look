# install.ps1 — LOOK Language XAMPP kurulum scripti (CGI modu)
# Kullanim: .\install.ps1 [-XamppDir "C:\xampp"]
# Not: Yonetici olarak calistir (Administrator PowerShell)
param(
    [string]$XamppDir = "C:\xampp"
)

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir   = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\..\..\cpp\build\Release"))
$CgiBinDir  = Join-Path $XamppDir "cgi-bin"
$HtdocsConf = Join-Path $XamppDir "apache\conf\httpd.conf"
$HtdocsDir  = Join-Path $XamppDir "htdocs"
$HttpdExe   = Join-Path $XamppDir "apache\bin\httpd.exe"

function Log($msg) { Write-Host "[LOOK] $msg" }
function Err($msg) { Write-Host "[HATA] $msg" -ForegroundColor Red; exit 1 }

# 1. Gerekli kontroller
if (-not (Test-Path $HtdocsConf)) { Err "httpd.conf bulunamadi: $HtdocsConf" }
if (-not (Test-Path $CgiBinDir))  { Err "cgi-bin klasoru bulunamadi: $CgiBinDir" }

$LkCgiSrc = Join-Path $BuildDir "lk-cgi.exe"
if (-not (Test-Path $LkCgiSrc)) { Err "lk-cgi.exe bulunamadi: $LkCgiSrc. Once build edin." }
Log "Binary kaynak: $BuildDir"

# 2. lk-cgi.exe kopyala
Copy-Item $LkCgiSrc (Join-Path $CgiBinDir "lk-cgi.exe") -Force
Log "Kopyalandi: lk-cgi.exe -> $CgiBinDir"

# lk.exe de kopyala (komut satiri icin)
$LkSrc = Join-Path $BuildDir "lk.exe"
if (Test-Path $LkSrc) {
    Copy-Item $LkSrc "C:\look\lk.exe" -Force -ErrorAction SilentlyContinue
}

# 3. httpd.conf — onceki LOOK blogunu temizle
$content = Get-Content $HtdocsConf -Raw -Encoding UTF8
if ($content -match '# --- LOOK Language') {
    $content = $content -replace '\r?\n\s*# --- LOOK Language[\s\S]*?# --- /LOOK Language ---', ''
    Log "Eski LOOK blogu silindi."
}

# mod_rewrite etkinlestir
if ($content -match '(?m)^#(LoadModule\s+rewrite_module\s+\S+)') {
    $content = $content -replace '(?m)^#(LoadModule\s+rewrite_module\s+\S+)', '$1'
    Log "mod_rewrite etkinlestirildi."
}

# 4. CGI blogunu Directory icine ekle (string birlestirme — $1 icermez)
$lookBlock = "
    # --- LOOK Language CGI ---
    Action look-handler /cgi-bin/lk-cgi.exe
    AddHandler look-handler .lk

    <IfModule mod_rewrite.c>
        RewriteEngine On
        RewriteCond %{REQUEST_FILENAME} -f [OR]
        RewriteCond %{REQUEST_FILENAME} -d
        RewriteRule ^ - [L]
        RewriteRule ^ /index.lk [L]
    </IfModule>
    # --- /LOOK Language ---"

$pattern = '(<Directory\s+"C:/xampp/htdocs">[\s\S]*?)(</Directory>)'
$m = [regex]::Match($content, $pattern)
if ($m.Success) {
    $insertAt = $m.Index + $m.Groups[1].Length
    $content = $content.Substring(0, $insertAt) + $lookBlock + "`n" + $content.Substring($insertAt)
    [System.IO.File]::WriteAllText($HtdocsConf, $content, [System.Text.Encoding]::UTF8)
    Log "httpd.conf yamalandi."
} else {
    Err "httpd.conf icinde <Directory C:/xampp/htdocs> blogu bulunamadi."
}

# 5. Syntax kontrol
$proc = Start-Process $HttpdExe -ArgumentList "-t" -NoNewWindow -PassThru `
    -RedirectStandardOutput "$env:TEMP\httpd_out.txt" `
    -RedirectStandardError  "$env:TEMP\httpd_err.txt" -Wait
$combined = "$(Get-Content "$env:TEMP\httpd_out.txt" -Raw -EA SilentlyContinue) $(Get-Content "$env:TEMP\httpd_err.txt" -Raw -EA SilentlyContinue)"
if ($combined -notmatch "Syntax OK") { Err "httpd.conf syntax hatasi:`n$combined" }
Log "httpd.conf syntax OK."

# 6. Test dosyasi olustur
$testFile = Join-Path $HtdocsDir "test.lk"
[System.IO.File]::WriteAllText($testFile, "print(`"Merhaba LOOK!`")`n", [System.Text.Encoding]::UTF8)
Log "Test dosyasi olusturuldu: $testFile"

Log ""
Log "Kurulum tamamlandi!"
Log "  -> XAMPP Apache'yi yeniden baslt."
Log "  -> Test: http://localhost/test.lk"
