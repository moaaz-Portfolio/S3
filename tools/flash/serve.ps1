$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $PSScriptRoot).Path.TrimEnd('\')
$RootPrefix = $Root + '\'
$Port = 8765
$Listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
try { $Listener.Start() } catch {
    Write-Host "تعذر فتح المنفذ $Port. أغلق أي نسخة أخرى من الأداة ثم حاول مرة أخرى." -ForegroundColor Red
    Read-Host 'اضغط Enter للإغلاق'
    exit 1
}
$BaseUrl = "http://127.0.0.1:$Port/"
Write-Host ''
Write-Host 'S3 Sense Studio يعمل الآن على:' -ForegroundColor Green
Write-Host $BaseUrl -ForegroundColor Cyan
Write-Host 'اترك هذه النافذة مفتوحة أثناء التفليش. اضغط Ctrl+C للإغلاق.' -ForegroundColor Yellow
Start-Process $BaseUrl
$Mime = @{
    '.html' = 'text/html; charset=utf-8'; '.htm' = 'text/html; charset=utf-8';
    '.json' = 'application/json; charset=utf-8'; '.bin' = 'application/octet-stream';
    '.js' = 'text/javascript; charset=utf-8'; '.css' = 'text/css; charset=utf-8';
    '.txt' = 'text/plain; charset=utf-8'; '.png' = 'image/png'; '.svg' = 'image/svg+xml'
}
function Send-Response($Stream, [int]$Code, [string]$Reason, [string]$Type, [byte[]]$Body, [bool]$HeadOnly) {
    $Headers = "HTTP/1.1 $Code $Reason`r`nContent-Type: $Type`r`nContent-Length: $($Body.Length)`r`nConnection: close`r`nCache-Control: no-store`r`nX-Content-Type-Options: nosniff`r`n`r`n"
    $HeaderBytes = [System.Text.Encoding]::ASCII.GetBytes($Headers)
    $Stream.Write($HeaderBytes, 0, $HeaderBytes.Length)
    if (-not $HeadOnly -and $Body.Length -gt 0) { $Stream.Write($Body, 0, $Body.Length) }
    $Stream.Flush()
}
try {
  while ($true) {
    $Client = $Listener.AcceptTcpClient()
    $Reader = $null
    try {
      $Stream = $Client.GetStream()
      $Reader = New-Object System.IO.StreamReader($Stream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
      $RequestLine = $Reader.ReadLine()
      if ([string]::IsNullOrWhiteSpace($RequestLine)) { $Client.Close(); continue }
      $Parts = $RequestLine.Split(' ')
      $Method = $Parts[0].ToUpperInvariant()
      $Target = $Parts[1].Split('?')[0]
      while ($true) { $HeaderLine = $Reader.ReadLine(); if ([string]::IsNullOrEmpty($HeaderLine)) { break } }
      if ($Method -ne 'GET' -and $Method -ne 'HEAD') {
        $Body = [System.Text.Encoding]::UTF8.GetBytes('Method not allowed')
        Send-Response $Stream 405 'Method Not Allowed' 'text/plain; charset=utf-8' $Body ($Method -eq 'HEAD')
        continue
      }
      $Relative = [Uri]::UnescapeDataString($Target.TrimStart('/'))
      if ([string]::IsNullOrWhiteSpace($Relative)) { $Relative = 'index.html' }
      $Candidate = [System.IO.Path]::GetFullPath((Join-Path $Root $Relative))
      if (-not $Candidate.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        $Body = [System.Text.Encoding]::UTF8.GetBytes('Forbidden')
        Send-Response $Stream 403 'Forbidden' 'text/plain; charset=utf-8' $Body ($Method -eq 'HEAD')
        continue
      }
      if (-not (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        $Body = [System.Text.Encoding]::UTF8.GetBytes('Not found')
        Send-Response $Stream 404 'Not Found' 'text/plain; charset=utf-8' $Body ($Method -eq 'HEAD')
        continue
      }
      $Body = [System.IO.File]::ReadAllBytes($Candidate)
      $Ext = [System.IO.Path]::GetExtension($Candidate).ToLowerInvariant()
      $Type = if ($Mime.ContainsKey($Ext)) { $Mime[$Ext] } else { 'application/octet-stream' }
      Send-Response $Stream 200 'OK' $Type $Body ($Method -eq 'HEAD')
    } catch {
      try { $Body = [System.Text.Encoding]::UTF8.GetBytes('Server error'); Send-Response $Stream 500 'Internal Server Error' 'text/plain; charset=utf-8' $Body $false } catch {}
    } finally {
      if ($Reader) { $Reader.Dispose() }
      $Client.Close()
    }
  }
} finally { $Listener.Stop() }
