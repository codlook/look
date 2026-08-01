# LOOK — Windows XAMPP paketi olustur (CGI modu).
# Calistirma: repo kokunden  powershell -ExecutionPolicy Bypass -File platforms\windows\xampp\build.ps1
#
# Gomulu .exe = MSVC Release build (cpp\build-win\Release): Schannel TLS (sifir dis DLL),
# bu oturumun fix'leri (int64/cache/channel/guvenlik). Once derle:
#   cmake -S cpp -B cpp\build-win -DCMAKE_BUILD_TYPE=Release
#   cmake --build cpp\build-win --config Release --target look look-cgi look-fcgi
$ErrorActionPreference = "Stop"
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Version = "1.0.0"
$Out     = Join-Path $Here "look-lang-xampp-$Version.zip"
$BinSrc  = [System.IO.Path]::GetFullPath((Join-Path $Here "..\..\..\cpp\build-win\Release"))
$Tmp     = Join-Path $env:TEMP ("xampp-pkg-" + [guid]::NewGuid().ToString("N"))

$exes    = @("lk.exe", "lk-cgi.exe", "lk-fcgi.exe")
$scripts = @("install.ps1", "install.bat", "uninstall.ps1", "patch_httpd.ps1", "README.md")

foreach ($b in $exes) {
    if (-not (Test-Path (Join-Path $BinSrc $b))) {
        Write-Error "HATA: $b yok ($BinSrc) - once MSVC build et (bkz. dosya basi)."
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $Tmp "bin") | Out-Null
foreach ($s in $scripts) { Copy-Item (Join-Path $Here $s) (Join-Path $Tmp $s) }
foreach ($b in $exes)    { Copy-Item (Join-Path $BinSrc $b) (Join-Path $Tmp "bin\$b") }

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $Tmp "*") -DestinationPath $Out
Remove-Item $Tmp -Recurse -Force

$mb = [math]::Round((Get-Item $Out).Length / 1MB, 1)
Write-Host "OK: platforms\windows\xampp\look-lang-xampp-$Version.zip ($mb MB)"
