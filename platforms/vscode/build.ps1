# LOOK — VSCode eklenti paketi (.vsix) olustur.
# Calistirma:  powershell -ExecutionPolicy Bypass -File platforms\vscode\build.ps1
# Gereksinim:  Node.js (npx ile @vscode/vsce indirilir).
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $Here
try {
    $pkg     = Get-Content (Join-Path $Here "package.json") -Raw | ConvertFrom-Json
    $version = $pkg.version
    $out     = [System.IO.Path]::GetFullPath((Join-Path $Here "..\..\releases\look-lang-$version.vsix"))
    New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null

    Write-Host "vsce package -> $out"
    & npx --yes @vscode/vsce package --no-dependencies --out $out
    if ($LASTEXITCODE -ne 0) { throw "vsce package basarisiz (exit $LASTEXITCODE)" }

    $mb = [math]::Round((Get-Item $out).Length / 1KB, 1)
    Write-Host "OK: releases\look-lang-$version.vsix ($mb KB)"
} finally {
    Pop-Location
}
