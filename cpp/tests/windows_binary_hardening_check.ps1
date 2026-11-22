# A Windows binary carries its memory-safety protections: it is loaded at a random address (Dynamic base,
# with the full 64-bit range: High Entropy Virtual Addresses), its data is not executable (NX compatible), and
# it is built with Control Flow Guard (an indirect call lands only on a valid function: "CF instrumented" and a
# non-empty Guard CF function table).
#
# The flags are set in CMakeLists.txt (LOOK_HARDEN, MSVC branch). A changed link line or a new library can drop
# one without any test noticing, so this reads the FINISHED binary with dumpbin. The Linux counterpart is
# binary_hardening_check.sh.
# Usage: pwsh cpp/tests/windows_binary_hardening_check.ps1 <exe> [<exe> ...]
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)
$ErrorActionPreference = "Stop"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: cannot locate dumpbin" }
$dumpbin = & $vswhere -latest -products * -find "VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe not found in the Visual Studio installation" }

$fail = $false
foreach ($f in $Files) {
    if (-not (Test-Path $f)) { Write-Host "  FAIL ${f}: no such file"; $fail = $true; continue }
    $name = Split-Path $f -Leaf
    $headers = (& $dumpbin /headers $f) -join "`n"
    $load = (& $dumpbin /loadconfig $f) -join "`n"
    $checks = @(
        @("Dynamic base (random load address)",                 $headers -match "Dynamic base"),
        @("High Entropy Virtual Addresses",                     $headers -match "High Entropy Virtual Addresses"),
        @("NX compatible (data is not executable)",             $headers -match "NX compatible"),
        @("Control Flow Guard in the image header",             $headers -match "Control Flow Guard"),
        @("CF instrumented",                                    $load -match "(?i)CF instrumented")
    )
    $count = 0
    if ($load -match "(?m)^\s*([0-9A-Fa-f]+)\s+Guard CF function count") { $count = [Convert]::ToInt64($Matches[1], 16) }
    $checks += , @("Guard CF function table is not empty ($count entries)", ($count -gt 0))
    foreach ($c in $checks) {
        if ($c[1]) { Write-Host "  OK   ${name}: $($c[0])" }
        else { Write-Host "  FAIL ${name}: $($c[0]) is missing"; $fail = $true }
    }
}
if ($fail) { Write-Host "FAIL: Windows binary hardening"; exit 1 }
Write-Host "PASS: Windows binary hardening"
