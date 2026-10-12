# TX A-MPDU test (ADDBA request) with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\ampdu.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\ampdu.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Par($extra, $url, $fmt) { $sw=[Diagnostics.Stopwatch]::StartNew(); $ps=@(); 1..4 | ForEach-Object { $of = Join-Path $env:TEMP "cu$_.txt"; Remove-Item $of -ErrorAction SilentlyContinue; $a = @("-sS","-k","-A","Mozilla/5.0","--max-time","25") + $extra + @("-o","NUL","-w",$fmt,$url); $ps += Start-Process -FilePath $curl -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput $of -RedirectStandardError (Join-Path $env:TEMP "cue$_.txt") }; $ps | Wait-Process -Timeout 40 -ErrorAction SilentlyContinue; $sw.Stop(); $n=0; $dbg=@(); 1..4 | ForEach-Object { $t = (Get-Content (Join-Path $env:TEMP "cu$_.txt") -Raw -ErrorAction SilentlyContinue); $e = (Get-Content (Join-Path $env:TEMP "cue$_.txt") -Raw -ErrorAction SilentlyContinue); $v = 0; if ($t -and [double]::TryParse(($t.Trim() -split '\s+')[0], [ref]$v)) { $n += $v }; if ($e) { $dbg += $e.Trim() } }; if ($dbg) { ("  err: " + ($dbg -join ' | ')) | Out-File $o -Append }; [pscustomobject]@{Bytes=$n; Sec=$sw.Elapsed.TotalSeconds} }
function Dl { $x = Par @() "https://speed.cloudflare.com/__down?bytes=25000000" "%{size_download}"; if ($x.Bytes -lt 20000000) { "invalid($([math]::Round($x.Bytes/1e6,1))MB)" } else { [math]::Round($x.Bytes*8/1e6/$x.Sec,1) } }
function Ul { $x = Par @("--data-binary","@$f") "https://speed.cloudflare.com/__up" "%{size_upload}"; if ($x.Bytes -lt 20000000) { "invalid($([math]::Round($x.Bytes/1e6,1))MB)" } else { [math]::Round($x.Bytes*8/1e6/$x.Sec,1) } }
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
