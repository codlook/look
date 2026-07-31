# uninstall.ps1 — LOOK Language'i XAMPP'tan kaldırır (install.ps1'i tersine çevirir)
# Kullanım: .\uninstall.ps1 [-XamppDir "C:\xampp"] [-KeepBackups] [-KeepTest]
# Not: Yönetici olarak çalıştır. Kullanıcının kendi .lk dosyalarına DOKUNMAZ.
param(
    [string]$XamppDir = "C:\xampp",
    [switch]$KeepBackups,   # .bak-* yedeklerini sil-me
    [switch]$KeepTest       # test.lk'yi sil-me
)
$ErrorActionPreference = "Stop"
$HtdocsConf = Join-Path $XamppDir "apache\conf\httpd.conf"
$CgiBinDir  = Join-Path $XamppDir "cgi-bin"
$HtdocsDir  = Join-Path $XamppDir "htdocs"
$HttpdExe   = Join-Path $XamppDir "apache\bin\httpd.exe"
function Log($m) { Write-Host "[LOOK] $m" }

# 1. httpd.conf — LOOK CGI blogunu sil
if (Test-Path $HtdocsConf) {
    $c = Get-Content $HtdocsConf -Raw -Encoding UTF8
    if ($c -match '# --- LOOK Language') {
        $c = $c -replace '\r?\n\s*# --- LOOK Language[\s\S]*?# --- /LOOK Language ---', ''
        [System.IO.File]::WriteAllText($HtdocsConf, $c, [System.Text.Encoding]::UTF8)
        Log "httpd.conf: LOOK CGI blogu silindi."
    } else { Log "httpd.conf: LOOK blogu yok." }
}

# 2. Binary'ler + yedekler
$targets = @(
    (Join-Path $CgiBinDir "lk-cgi.exe"),
    (Join-Path $XamppDir  "php\look.exe"),
    (Join-Path $XamppDir  "php\lk.exe")
)
foreach ($t in $targets) { if (Test-Path $t) { Remove-Item $t -Force; Log "silindi: $t" } }
if (-not $KeepBackups) {
    Get-ChildItem (Join-Path $CgiBinDir "lk-cgi.exe.bak-*"),(Join-Path $XamppDir "php\look.exe.bak-*") -EA SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force; Log "yedek silindi: $($_.Name)" }
} else { Log "yedekler korundu (-KeepBackups)." }

# 3. Kurulum artefaktlari (kullanıcının kendi dosyalarına DOKUNMA)
Remove-Item (Join-Path $HtdocsDir ".look_cache") -Recurse -Force -EA SilentlyContinue
if (-not $KeepTest) { Remove-Item (Join-Path $HtdocsDir "test.lk") -Force -EA SilentlyContinue; Log "test.lk silindi." }
Log "NOT: kendi .lk dosyaların ve loglar korundu."

# 4. Apache reload (httpd -t "Syntax OK"'i stderr'e yazar → 2>&1 + Stop ile
#    yanlış hata sayılır; native çağrıları try/catch ile izole et)
if (Test-Path $HttpdExe) {
    try { Start-Process $HttpdExe -ArgumentList "-k","restart" -WindowStyle Hidden -Wait } catch {}
    Log "Apache yeniden yuklendi."
}
Log ""
Log "LOOK XAMPP'tan kaldirildi. Artik .lk dosyalari Apache tarafindan islenmez."
