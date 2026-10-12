# TX A-MPDU test (ADDBA request) with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\ampdu.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\ampdu.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Sum($lines) { $n = 0; foreach ($x in (("$($lines -join ' ')") -split '\s+')) { $v = 0; if ($x -match '^\d{7,}$' -and [double]::TryParse($x, [ref]$v)) { $n += $v } }; $n }
function Dl { $urls=@("https://speed.cloudflare.com/__down?bytes=25000000","https://speed.hetzner.de/100MB.bin","http://ipv4.download.thinkbroadband.com/100MB.zip"); foreach ($u in $urls) { $sw=[Diagnostics.Stopwatch]::StartNew(); $r = & $curl -sS -L -k -A "Mozilla/5.0" --max-time 15 -Z --parallel-max 4 -w "%{size_download} %{http_code}`n" -o NUL $u -o NUL $u -o NUL $u -o NUL $u 2>&1; $sw.Stop(); $by = Sum ($r | ForEach-Object { "$_" }); if ($by -ge 20000000) { return [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) }; ("  dl-debug " + $u.Substring(0,30) + " exit=$LASTEXITCODE out=" + (($r | ForEach-Object { "$_" }) -join ' | ')) | Tee-Object -FilePath $o -Append | Out-Null }; "invalid" }
function Ul { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="https://speed.cloudflare.com/__up"; $r = & $curl -s -L -A "Mozilla/5.0" -Z --parallel-max 4 -w "%{size_upload} %{http_code}`n" --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 38000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
"== ampdu test $(Get-Date) ==" | Out-File $o
foreach ($v in @(@{a=0;t="TX A-MPDU OFF"}, @{a=1;t="TX A-MPDU ON (ADDBA request + AGG_EN)"})) {
  Set-ItemProperty $k -Name Cfg_TxAmpdu -Value $v.a -Type DWord
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  for ($i=0; $i -lt 20; $i++) { Start-Sleep 3; if (((netsh wlan show interfaces | Select-String '^\s+State').ToString()) -match ': connected') { break } }
  Start-Sleep 8
  "--- $($v.t) ---" | Tee-Object -FilePath $o -Append
  for ($r=1; $r -le 3; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
  Get-ItemProperty $k | Select-Object Log_TxAmpdu_*,Log_TxPipe_*,Log_Data_TxOk,Log_Data_TxDrop,Log_Data_TxFail,Log_Dp_RxRingDrop | Format-List | Out-File $o -Append
}
Set-ItemProperty $k -Name Cfg_TxAmpdu -Value 1 -Type DWord
Remove-Item $f -ErrorAction SilentlyContinue
"Done -> $o"
