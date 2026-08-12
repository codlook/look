# Windows Tier-2 proof: lk-fcgi.exe --mode http on Windows
#   (a) returns 200 + expected body to an HTTP request,
#   (b) closes an idle keep-alive connection at timeout (Read=0).
# Does NOT modify source -- only exercises the real MSVC binary.
# Positive control: wrong port cannot connect; expected content (not just 200)
# is verified; idle-close is proven live-BEFORE (got 200) then closed-AFTER (Read=0).
# The process is killed by PID (never a broad kill).
param(
  [string]$Exe    = "cpp/build-win/Release/lk-fcgi.exe",
  [string]$Script = "cpp/tests/win_http_idle_app.lk",
  [int]   $Port   = 7788
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe))    { throw "binary missing: $Exe" }
if (-not (Test-Path $Script)) { throw "app missing: $Script" }

$proc = Start-Process -FilePath $Exe `
          -ArgumentList @("--mode","http","--port","$Port",$Script) `
          -PassThru -WindowStyle Hidden
Write-Host "lk-fcgi PID=$($proc.Id) port=$Port"

try {
  # 1) wait until ready (up to ~10s)
  $ready = $false
  for ($i=0; $i -lt 50; $i++) {
    try {
      $t = New-Object System.Net.Sockets.TcpClient
      $t.Connect("127.0.0.1",$Port); $t.Close(); $ready = $true; break
    } catch { Start-Sleep -Milliseconds 200 }
  }
  if (-not $ready) { throw "server not ready on port $Port" }

  # 2) POSITIVE CONTROL (negative): wrong port must refuse
  $wrong = $Port + 11
  $refused = $false
  try { $w = New-Object System.Net.Sockets.TcpClient; $w.Connect("127.0.0.1",$wrong); $w.Close() }
  catch { $refused = $true }
  if (-not $refused) { throw "negative-control FAILED: wrong port $wrong connected" }
  Write-Host "OK negative-control: wrong port $wrong refused"

  # 3) 200 + expected body (raw TCP keep-alive)
  $c = New-Object System.Net.Sockets.TcpClient
  $c.Connect("127.0.0.1",$Port)
  $s = $c.GetStream()
  $req = "GET / HTTP/1.1`r`nHost: x`r`nConnection: keep-alive`r`n`r`n"
  $b = [Text.Encoding]::ASCII.GetBytes($req)
  $buf = New-Object byte[] 4096
  $s.Write($b,0,$b.Length); $s.Flush()
  $n = $s.Read($buf,0,$buf.Length)     # first response -- proves live keep-alive conn
  if ($n -le 0) { throw "no response (conn closed early)" }
  $resp = [Text.Encoding]::ASCII.GetString($buf,0,$n)
  $line1 = ($resp -split "`r`n")[0]
  if ($line1 -notmatch "^HTTP/1\.1 200") { throw "expected 200, got: $line1" }
  # POSITIVE CONTROL: content actually came from the route (wrong content = RED)
  if ($resp -notmatch "LOOK-WIN-HTTP-OK") { throw "expected body 'LOOK-WIN-HTTP-OK' missing -- server not exercised" }
  Write-Host "OK 200 + body verified: $line1"

  # 4) idle-timeout: connection is live NOW (we got 200). Leave idle -> server must FIN.
  #    Blocking Read returns 0 when server closes. 40s ceiling covers both SO_RCVTIMEO
  #    interpretations (30ms / 30s); if not closed by then, idle-timeout is NOT working.
  $c.ReceiveTimeout = 40000
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $closed = $false
  try {
    $n2 = $s.Read($buf,0,$buf.Length)
    if ($n2 -eq 0) { $closed = $true }
    else { throw "unexpected $n2 bytes on idle connection" }
  } catch [System.IO.IOException] {
    throw "idle-timeout did not fire within 40s (server did not close idle keep-alive)"
  }
  $sw.Stop()
  $c.Close()
  if (-not $closed) { throw "idle-close not observed" }
  Write-Host ("OK idle-timeout: server closed idle keep-alive after {0:N0} ms (Read=0)" -f $sw.Elapsed.TotalMilliseconds)

  Write-Host "PASS: Windows --mode http (200 + idle-timeout close)"
}
finally {
  if ($proc -and -not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force
    Write-Host "lk-fcgi PID=$($proc.Id) stopped"
  }
}
