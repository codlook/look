# install.ps1 — LOOK Language XAMPP kurulum scripti (CGI modu)
# Kullanim: .\install.ps1 [-XamppDir "C:\xampp"]
# Not: Yonetici olarak calistir (Administrator PowerShell)
param(
    [string]$XamppDir = "C:\xampp"
)

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
# Binary kaynagi: once script'in yanindaki bin\ (release zip — self-contained),
# sonra repo build dizinleri (build-win/MSVC once). -BuildDir ile override.
$BuildDir   = $null
if (Test-Path (Join-Path $ScriptDir "bin\lk-cgi.exe")) {
    $BuildDir = (Join-Path $ScriptDir "bin")
} else {
    foreach ($d in @("build-win\Release","build\Release","build-win","build")) {
        $p = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\..\..\cpp\$d"))
        if (Test-Path (Join-Path $p "lk-cgi.exe")) { $BuildDir = $p; break }
    }
}
if (-not $BuildDir) { $BuildDir = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\..\..\cpp\build-win\Release")) }
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

# 2. lk-cgi.exe kopyala (eskisini yedekle — geri dönülebilir)
$LkCgiDst = Join-Path $CgiBinDir "lk-cgi.exe"
if (Test-Path $LkCgiDst) {
    $ts = Get-Date -Format "yyyyMMdd-HHmmss"
    Copy-Item $LkCgiDst "$LkCgiDst.bak-$ts" -Force
    Log "Eski lk-cgi.exe yedeklendi: lk-cgi.exe.bak-$ts"
}
Copy-Item $LkCgiSrc $LkCgiDst -Force
Log "Kopyalandi: lk-cgi.exe -> $CgiBinDir"
# Eski .look_cache temizle (stale bytecode)
Remove-Item (Join-Path $HtdocsDir ".look_cache") -Recurse -Force -ErrorAction SilentlyContinue

# lk.exe de kopyala (komut satiri icin). C:\look YOKSA once olustur — aksi halde
# $ErrorActionPreference=Stop ile Copy-Item DirectoryNotFound firlatir ve kurulumu
# httpd.conf yamasindan ONCE durdurur (binary kopyalanmis ama handler yok = calismaz).
# Bu kopya ISTEGE BAGLI (yalniz CLI kolayligi) → hatasi kurulumu ASLA durdurmamali.
# Gercek XAMPP testinde bulundu (2026-08-02): temiz makinede C:\look yoktu, kurulum yarim kaldi.
$LkSrc = Join-Path $BuildDir "lk.exe"
if (Test-Path $LkSrc) {
    try {
        New-Item -ItemType Directory -Force "C:\look" | Out-Null
        Copy-Item $LkSrc "C:\look\lk.exe" -Force
    } catch {
        Log "uyari: C:\look\lk.exe kopyalanamadi (CLI lk.exe atlandi, kurulum devam ediyor): $($_.Exception.Message)"
    }
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

# 6. Test dosyasi olustur (v1 idiom — fn + response::json + response::error)
$testFile = Join-Path $HtdocsDir "test.lk"
$testApp = @'
route("GET", "/", fn() => response::json(["ok" => true, "mesaj" => "Merhaba LOOK v1!"]))
route("GET", "/topla/{a}/{b}", function($a, $b) { response::json(["toplam" => int($a) + int($b)]) })
route("404", fn() => response::error(404, "Bulunamadı"))
'@
[System.IO.File]::WriteAllText($testFile, $testApp, (New-Object System.Text.UTF8Encoding($false)))
Log "Test dosyasi olusturuldu: $testFile"

Log ""
Log "Kurulum tamamlandi!"
Log "  -> XAMPP Apache'yi yeniden baslt."
Log "  -> Test: http://localhost/test.lk"
