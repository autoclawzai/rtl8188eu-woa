# Speed with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\pipe.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\pipe.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Sum($lines) { $n = 0; foreach ($x in (("$($lines -join ' ')") -split '\s+')) { $v = 0; if ($x -match '^\d{7,}$' -and [double]::TryParse($x, [ref]$v)) { $n += $v } }; $n }
function Dl { $urls=@("https://speed.cloudflare.com/__down?bytes=25000000","https://speed.hetzner.de/100MB.bin","http://ipv4.download.thinkbroadband.com/100MB.zip"); foreach ($u in $urls) { $sw=[Diagnostics.Stopwatch]::StartNew(); $r = & $curl -sS -L -k -A "Mozilla/5.0" --max-time 15 -Z --parallel-max 4 -w "%{size_download} %{http_code}`n" -o NUL $u -o NUL $u -o NUL $u -o NUL $u 2>&1; $sw.Stop(); $by = Sum ($r | ForEach-Object { "$_" }); if ($by -ge 20000000) { return [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) }; ("  dl-debug " + $u.Substring(0,30) + " exit=$LASTEXITCODE out=" + (($r | ForEach-Object { "$_" }) -join ' | ')) | Tee-Object -FilePath $o -Append | Out-Null }; "invalid" }
function Ul { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="https://speed.cloudflare.com/__up"; $r = & $curl -s -L -A "Mozilla/5.0" -Z --parallel-max 4 -w "%{size_upload} %{http_code}`n" --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 38000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
"== pipe test $(Get-Date) ==" | Out-File $o
for ($r=1; $r -le 3; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
Get-ItemProperty $k | Select-Object Log_TxPipe_*,Log_TxAsync_*,Log_Data_TxOk,Log_Data_TxDrop,Log_Dp_TxAdv,Log_Dp_TxPkt,Log_Dp_RxRingDrop,Log_Data_Rx,Log_Rate_PickMcs7,Log_Ba_Req | Format-List | Out-File $o -Append
Remove-Item $f -ErrorAction SilentlyContinue
"Done -> $o"
