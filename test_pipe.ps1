# Speed with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\pipe.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\pipe.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Sum($lines) { $n = 0; foreach ($x in @($lines)) { $v = 0; if ([double]::TryParse("$x".Trim(), [ref]$v)) { $n += $v } }; $n }
function Dl { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="http://speed.cloudflare.com/__down?bytes=12000000"; $r = & $curl -s -Z --parallel-max 4 -w "%{size_download}\n" -o NUL $u -o NUL $u -o NUL $u -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 40000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
function Ul { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="http://speed.cloudflare.com/__up"; $r = & $curl -s -Z --parallel-max 4 -w "%{size_upload}\n" --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 38000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
"== pipe test $(Get-Date) ==" | Out-File $o
for ($r=1; $r -le 3; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
Get-ItemProperty $k | Select-Object Log_TxPipe_*,Log_TxAsync_*,Log_Data_TxOk,Log_Data_TxDrop,Log_Dp_TxAdv,Log_Dp_TxPkt,Log_Dp_RxRingDrop,Log_Data_Rx,Log_Rate_PickMcs7,Log_Ba_Req | Format-List | Out-File $o -Append
Remove-Item $f -ErrorAction SilentlyContinue
"Done -> $o"
