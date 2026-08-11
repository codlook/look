# LOOK - Windows binary package (plain binaries, NO installer script).
# XAMPP plumbing is deliberately absent: LOOK has its own HTTP server
# (lk-fcgi.exe --mode http), so no Apache / integration is needed. See README.md.
#
# First do an MSVC Release build:
#   cmake -S cpp -B cpp\build-win -DLOOK_BUILD=<sha>
#   cmake --build cpp\build-win --config Release --target look look-fcgi
# Then: powershell -ExecutionPolicy Bypass -File platforms\windows\build.ps1
$ErrorActionPreference = "Stop"
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Version = "1.0.0"
$Out     = Join-Path $Here "look-lang-windows-$Version.zip"
$BinSrc  = [System.IO.Path]::GetFullPath((Join-Path $Here "..\..\cpp\build-win\Release"))
$Tmp     = Join-Path $env:TEMP ("win-pkg-" + [guid]::NewGuid().ToString("N"))

# lk-fcgi.exe = HTTP server (the whole point); lk.exe = CLI / script / REPL.
# lk-cgi.exe is NOT included: --mode http needs no integration; CGI is advanced use.
$exes = @("lk.exe", "lk-fcgi.exe")
foreach ($b in $exes) {
    if (-not (Test-Path (Join-Path $BinSrc $b))) {
        Write-Error "MISSING: $b not found in $BinSrc - build MSVC Release first."
    }
}

# -- Pre-release verification gate (Windows analog of release_gate.lk) ------------
# Windows Tier-2 has NO CI, so this gate is the only regression protection. The
# MSVC-only branches (NCrypt rs256, BCrypt random/sha256, MSVC i64-overflow, IOCP)
# run ONLY here. If any is red, packaging STOPS (no silent hole gets shipped). Tests
# run against the real MSVC binary -- the sole proof rs256/crypto/date work on Windows.
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads .ps1 as ANSI).
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $Here "..\.."))
$LkExe    = Join-Path $BinSrc "lk.exe"
$Gate = @(
    "tests\crypto_vectors.lk",   # SHA/HMAC/base64 + BCrypt random (NIST/RFC)
    "tests\rs256_kat.lk",        # NCrypt rs256 verify+sign, pinned to openssl oracle
    "tests\date_dst_test.lk"     # calendar arithmetic (MSVC CRT branch)
)
Write-Host "Pre-release gate (against MSVC binary)..."
foreach ($t in $Gate) {
    $tp = Join-Path $RepoRoot (Join-Path "cpp" $t)
    & $LkExe $tp | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "GATE RED: $t FAILED on Windows binary (exit $LASTEXITCODE) - packaging stopped."
    }
    Write-Host "  PASS $t"
}
Write-Host "Gate green: MSVC branches (rs256/crypto/date) verified."

New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
foreach ($b in $exes) { Copy-Item (Join-Path $BinSrc $b) (Join-Path $Tmp $b) }
Copy-Item (Join-Path $Here "README.md") (Join-Path $Tmp "README.md")

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $Tmp "*") -DestinationPath $Out
Remove-Item $Tmp -Recurse -Force

$mb = [math]::Round((Get-Item $Out).Length / 1MB, 1)
Write-Host "OK: look-lang-windows-$Version.zip - $mb MB"
Write-Host "Contents: lk.exe + lk-fcgi.exe + README.md (NO installer script)"
