# start-rabbids-web.ps1 - starts the Rabbids Go Home web port on this PC (only 127.0.0.1 / localhost).
# Usage: right click -> "Run with PowerShell", or:
#   powershell -ExecutionPolicy Bypass -File .\start-rabbids-web.ps1 [-Game "C:\path\entire partition"] [-Port 8080]
# -Game: folder with YOUR extracted copy of the game (the one with sys\, files\, tmd.bin). Default: ..\entire partition
# Nothing is uploaded anywhere: it only serves this folder and your copy of the game to your own browser.
param([Alias('Juego')][string]$Game = "", [Alias('Puerto')][int]$Port = 8080, [Alias('SinNavegador')][switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
$here = (Resolve-Path $PSScriptRoot).Path
$web = Join-Path $here 'web'
if (-not $Game) {
  # look for "entire partition" next to this folder or in the folders above it (up to 4 levels)
  $d = $here
  for ($i = 0; $i -lt 5 -and $d; $i++) {
    $c = Join-Path $d 'entire partition'
    if (Test-Path (Join-Path $c 'sys\boot.bin')) { $Game = $c; break }
    $d = Split-Path $d -Parent
  }
  if (-not $Game) { $Game = Join-Path (Split-Path $here -Parent) 'entire partition' }
}
if (-not (Test-Path (Join-Path $Game 'sys\boot.bin'))) {
  Write-Host "Game copy not found at: $Game" -ForegroundColor Red
  Write-Host "Give the extracted folder with -Game ""C:\path\entire partition"" (it must contain sys\boot.bin and files\)." -ForegroundColor Yellow
  Read-Host 'Press Enter to exit'; exit 1
}
if (-not (Test-Path (Join-Path $web 'rgh.wasm')) -and -not (Test-Path (Join-Path $web 'rgh.wasm.gz'))) { Write-Host "web\rgh.wasm is missing (build it first: see README.md)" -ForegroundColor Red; Read-Host 'Enter'; exit 1 }
$url = "http://localhost:$Port/"
Write-Host "Game: $Game"
Write-Host "Open $url in Chrome or Edge (close this window to stop)." -ForegroundColor Cyan
if (-not $NoBrowser) { Start-Job -ScriptBlock { param($u) Start-Sleep -Seconds 2; Start-Process $u } -ArgumentList $url | Out-Null }

# Python (if installed) is faster; otherwise the built-in PowerShell server is used.
$py = Get-Command python -ErrorAction SilentlyContinue
if ($py -and (Test-Path (Join-Path $here 'serve.py'))) {
  & $py.Source -c "import sys; sys.exit(0 if sys.version_info >= (3,7) else 1)" 2>$null
  if ($LASTEXITCODE -eq 0) { & $py.Source (Join-Path $here 'serve.py') --web $web --game $Game --port $Port; exit }
}

$mime = @{ '.html'='text/html; charset=utf-8'; '.js'='text/javascript'; '.wasm'='application/wasm'; '.json'='application/json';
           '.png'='image/png'; '.jpg'='image/jpeg'; '.css'='text/css'; '.txt'='text/plain; charset=utf-8' }
# ---- DSU bridge (cemuhook): the browser cannot use UDP; this server talks to the phone app
$script:dsu = $null; $script:dsuEp = $null; $script:dsuPkts = @($null, $null, $null, $null); $script:dsuTimes = @([DateTime]::MinValue, [DateTime]::MinValue, [DateTime]::MinValue, [DateTime]::MinValue); $script:dsuRx = [DateTime]::MinValue; $script:dsuReq = [DateTime]::MinValue
$script:dsuCid = [uint32](Get-Random -Maximum 2147483647)
$crcTable = New-Object 'uint32[]' 256
for ($n = 0; $n -lt 256; $n++) { $c = [uint32]$n; for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [uint32](([uint32]3988292384) -bxor ($c -shr 1)) } else { $c = [uint32]($c -shr 1) } }; $crcTable[$n] = $c }
function Get-Crc32([byte[]]$b) { $c = [uint32]::MaxValue; foreach ($x in $b) { $c = [uint32]($crcTable[($c -bxor $x) -band 0xFF] -bxor ($c -shr 8)) }; return [uint32]($c -bxor [uint32]::MaxValue) }
function New-DsuPacket([uint32]$type, [byte[]]$payload) {
  $body = [BitConverter]::GetBytes($type) + $payload
  $p = [byte[]](([Text.Encoding]::ASCII.GetBytes('DSUC')) + [BitConverter]::GetBytes([uint16]1001) + [BitConverter]::GetBytes([uint16]$body.Length) + [BitConverter]::GetBytes([uint32]0) + [BitConverter]::GetBytes($script:dsuCid) + $body)
  [Array]::Copy([BitConverter]::GetBytes((Get-Crc32 $p)), 0, $p, 8, 4); return ,$p
}
function Test-LanIp([string]$h) {
  $ip = $null; if (-not [Net.IPAddress]::TryParse($h, [ref]$ip) -or $ip.AddressFamily -ne 'InterNetwork') { return $false }
  $b = $ip.GetAddressBytes()
  return ($b[0] -eq 10) -or ($b[0] -eq 127) -or ($b[0] -eq 192 -and $b[1] -eq 168) -or ($b[0] -eq 172 -and $b[1] -ge 16 -and $b[1] -le 31) -or ($b[0] -eq 169 -and $b[1] -eq 254) -or ($b[0] -eq 100 -and $b[1] -ge 64 -and $b[1] -le 127)
}
function Step-Dsu {
  if (-not $script:dsu) { return }
  try {
    if (((Get-Date) - $script:dsuReq).TotalSeconds -gt 1) {
      $a = New-DsuPacket 0x100001 ([byte[]](4,0,0,0,0,1,2,3)); [void]$script:dsu.Send($a, $a.Length, $script:dsuEp)
      $b = New-DsuPacket 0x100002 (New-Object byte[] 8); [void]$script:dsu.Send($b, $b.Length, $script:dsuEp)
      $script:dsuReq = Get-Date
    }
    while ($script:dsu.Available -gt 0) {
      $from = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
      $d = $script:dsu.Receive([ref]$from)
      if ($d.Length -ge 100 -and $d[0] -eq 0x44 -and $d[3] -eq 0x53 -and [BitConverter]::ToUInt32($d, 16) -eq 0x100002 -and $d[31] -ne 0) {
        $sl = $d[20] -band 3; $script:dsuPkts[$sl] = [byte[]]$d[0..99]; $script:dsuTimes[$sl] = Get-Date
      }
    }
  } catch { }
}
function Send-Bytes($res, [int]$code, [string]$type, [byte[]]$body) {
  $res.StatusCode = $code; $res.ContentType = $type; $res.ContentLength64 = $body.Length
  if ($body.Length) { $res.OutputStream.Write($body, 0, $body.Length) }; $res.Close()
}

$webBase = [IO.Path]::GetFullPath($web).TrimEnd('\') + '\'
$gameBase = [IO.Path]::GetFullPath($Game).TrimEnd('\') + '\'
$l = New-Object System.Net.HttpListener
$l.Prefixes.Add($url)
try { $l.Start() } catch { Write-Host "Could not open port $Port. Try: -Port 8090" -ForegroundColor Red; Read-Host 'Enter'; exit 1 }
try {
  while ($l.IsListening) {
    $ar = $l.BeginGetContext($null, $null)
    while (-not $ar.AsyncWaitHandle.WaitOne(3)) { Step-Dsu }
    $ctx = $l.EndGetContext($ar)
    Step-Dsu
    try {
      $rel = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
      if ($rel -eq '') { $rel = 'index.html' }
      if ($rel -match '(^|/)\.\.(/|$)') { $ctx.Response.StatusCode = 400; $ctx.Response.Close(); continue }
      if ($rel.StartsWith('game/')) { $base = $gameBase; $rel = $rel.Substring(5) } else { $base = $webBase }
      $path = [IO.Path]::GetFullPath((Join-Path $base ($rel.Replace('/', '\'))))
      $res = $ctx.Response
      $res.Headers.Add('Cross-Origin-Opener-Policy', 'same-origin')
      $res.Headers.Add('Cross-Origin-Embedder-Policy', 'require-corp')
      $res.Headers.Add('Cross-Origin-Resource-Policy', 'same-origin')
      $res.Headers.Add('Accept-Ranges', 'bytes')
      $res.Headers.Add('Cache-Control', 'no-cache')
      if ($rel -eq 'nand' -and $ctx.Request.HttpMethod -eq 'POST') {
        # backup of the browser's save data: rabbids-save-backup.bin next to web\
        $dst = Join-Path (Split-Path $webBase.TrimEnd('\') -Parent) 'rabbids-save-backup.bin'
        $ms = New-Object IO.MemoryStream; $ctx.Request.InputStream.CopyTo($ms)
        if ($ms.Length -gt 0 -and $ms.Length -le 67108864) { try { [IO.File]::WriteAllBytes($dst, $ms.ToArray()) } catch { } }
        Send-Bytes $res 200 'text/plain' ([Text.Encoding]::UTF8.GetBytes('ok')); continue
      }
      if ($rel -eq 'log' -and $ctx.Request.HttpMethod -eq 'POST') {
        # the page's log, saved as rabbids-log.txt next to web\ (new=1 starts a new file)
        $logf = Join-Path (Split-Path $webBase.TrimEnd('\') -Parent) 'rabbids-log.txt'
        $ms = New-Object IO.MemoryStream; $ctx.Request.InputStream.CopyTo($ms); $bytes = $ms.ToArray()
        if ($bytes.Length -gt 1048576) { $bytes = $bytes[0..1048575] }
        try {
          if ($ctx.Request.Url.Query -match 'new=1' -and (Test-Path -LiteralPath $logf)) { Move-Item -LiteralPath $logf -Destination ($logf -replace '\.txt$', '-previous.txt') -Force }
          if (-not (Test-Path -LiteralPath $logf) -or (Get-Item -LiteralPath $logf).Length -lt 31457280) { $fsl = [IO.File]::Open($logf, 'Append', 'Write', 'ReadWrite'); $fsl.Write($bytes, 0, $bytes.Length); $fsl.Close() }
        } catch { }
        Send-Bytes $res 200 'text/plain' ([Text.Encoding]::UTF8.GetBytes('ok')); continue
      }
      if ($rel.StartsWith('dsu/')) {
        $q = $ctx.Request.QueryString
        if ($rel -eq 'dsu/connect') {
          $h = [string]$q['host']; $pt = 26760; [void][int]::TryParse([string]$q['port'], [ref]$pt)
          if (-not (Test-LanIp $h) -or $pt -lt 1 -or $pt -gt 65535) { Send-Bytes $res 400 'application/json' ([Text.Encoding]::UTF8.GetBytes('{"ok":false,"error":"invalid IP (only addresses of your local network)"}')); continue }
          if ($script:dsu) { $script:dsu.Close() }
          $script:dsu = New-Object Net.Sockets.UdpClient(0); $script:dsuEp = New-Object Net.IPEndPoint([Net.IPAddress]::Parse($h), $pt)
          $script:dsuPkts = @($null, $null, $null, $null); $script:dsuReq = [DateTime]::MinValue
          Write-Host "DSU: connecting to ${h}:$pt"
          Send-Bytes $res 200 'application/json' ([Text.Encoding]::UTF8.GetBytes('{"ok":true}')); continue
        }
        if ($rel -eq 'dsu/disconnect') { if ($script:dsu) { $script:dsu.Close() }; $script:dsu = $null; $script:dsuPkts = @($null, $null, $null, $null); Send-Bytes $res 200 'application/json' ([Text.Encoding]::UTF8.GetBytes('{"ok":true}')); continue }
        if ($rel -eq 'dsu/state') {
          if (-not $script:dsu) { Send-Bytes $res 404 'application/octet-stream' (New-Object byte[] 0); continue }
          $all = New-Object System.Collections.Generic.List[byte]
          for ($k = 0; $k -lt 4; $k++) { if ($script:dsuPkts[$k] -and ((Get-Date) - $script:dsuTimes[$k]).TotalSeconds -lt 3) { $all.AddRange($script:dsuPkts[$k]) } }
          if ($all.Count -eq 0) { Send-Bytes $res 204 'application/octet-stream' (New-Object byte[] 0); continue }
          Send-Bytes $res 200 'application/octet-stream' $all.ToArray(); continue
        }
        Send-Bytes $res 404 'text/plain' (New-Object byte[] 0); continue
      }
      if (-not $path.StartsWith($base, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $res.StatusCode = 404; $res.Close(); continue }
      $ext = [IO.Path]::GetExtension($path).ToLower()
      $res.ContentType = $(if ($mime[$ext]) { $mime[$ext] } else { 'application/octet-stream' })
      $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
      try {
        $size = $fs.Length; $start = 0; $end = $size - 1
        $range = $ctx.Request.Headers['Range']
        if ($range -and $range -match 'bytes=(\d*)-(\d*)') {
          if ($Matches[1]) { $start = [int64]$Matches[1] }
          if ($Matches[2]) { $end = [Math]::Min([int64]$Matches[2], $size - 1) }
          $res.StatusCode = 206
          $res.Headers.Add('Content-Range', "bytes $start-$end/$size")
        }
        $len = $end - $start + 1
        $res.ContentLength64 = $len
        if ($ctx.Request.HttpMethod -ne 'HEAD') {
          $fs.Seek($start, 'Begin') | Out-Null
          $buf = New-Object byte[] 1048576
          while ($len -gt 0) {
            $n = $fs.Read($buf, 0, [Math]::Min($buf.Length, $len))
            if ($n -le 0) { break }
            $res.OutputStream.Write($buf, 0, $n); $len -= $n
          }
        }
      } finally { $fs.Dispose() }
      $res.Close()
    } catch { try { $ctx.Response.Abort() } catch {} }
  }
} finally { $l.Stop() }
