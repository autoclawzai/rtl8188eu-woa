# Speed with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\pipe.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\pipe.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Par($extra, $url, $fmt) { $sw=[Diagnostics.Stopwatch]::StartNew(); $ps=@(); 1..4 | ForEach-Object { $of = Join-Path $env:TEMP "cu$_.txt"; Remove-Item $of -ErrorAction SilentlyContinue; $a = @("-sS","-k","-A","Mozilla/5.0","--max-time","25") + $extra + @("-o","NUL","-w",$fmt,$url); $ps += Start-Process -FilePath $curl -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput $of -RedirectStandardError (Join-Path $env:TEMP "cue$_.txt") }; $ps | Wait-Process -Timeout 40 -ErrorAction SilentlyContinue; $sw.Stop(); $n=0; $dbg=@(); 1..4 | ForEach-Object { $t = (Get-Content (Join-Path $env:TEMP "cu$_.txt") -Raw -ErrorAction SilentlyContinue); $e = (Get-Content (Join-Path $env:TEMP "cue$_.txt") -Raw -ErrorAction SilentlyContinue); $v = 0; if ($t -and [double]::TryParse(($t.Trim() -split '\s+')[0], [ref]$v)) { $n += $v }; if ($e) { $dbg += $e.Trim() } }; if ($dbg) { ("  err: " + ($dbg -join ' | ')) | Out-File $o -Append }; [pscustomobject]@{Bytes=$n; Sec=$sw.Elapsed.TotalSeconds} }
function Dl { $x = Par @() "https://speed.cloudflare.com/__down?bytes=25000000" "%{size_download}"; if ($x.Bytes -lt 20000000) { "invalid($([math]::Round($x.Bytes/1e6,1))MB)" } else { [math]::Round($x.Bytes*8/1e6/$x.Sec,1) } }
function Ul { $x = Par @("--data-binary","@$f") "https://speed.cloudflare.com/__up" "%{size_upload}"; if ($x.Bytes -lt 20000000) { "invalid($([math]::Round($x.Bytes/1e6,1))MB)" } else { [math]::Round($x.Bytes*8/1e6/$x.Sec,1) } }
"== pipe test $(Get-Date) ==" | Out-File $o
for ($r=1; $r -le 3; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
Get-ItemProperty $k | Select-Object Log_TxPipe_*,Log_TxAsync_*,Log_Data_TxOk,Log_Data_TxDrop,Log_Dp_TxAdv,Log_Dp_TxPkt,Log_Dp_RxRingDrop,Log_Data_Rx,Log_Rate_PickMcs7,Log_Ba_Req | Format-List | Out-File $o -Append
Remove-Item $f -ErrorAction SilentlyContinue
"Done -> $o"
