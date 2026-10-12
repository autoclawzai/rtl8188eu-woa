# TX A-MPDU test (ADDBA request) with curl.exe (4 parallel streams, like a browser speedtest; Invoke-WebRequest is CPU-bound on the Pi and under-reads)
# + TX pipeline counters. Administrator PowerShell. Output: Desktop\ampdu.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\ampdu.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { "curl.exe nahi mila" | Tee-Object $o; return }
$f = Join-Path $env:TEMP "up10.bin"; $b = New-Object byte[] 10000000; (New-Object Random).NextBytes($b); [IO.File]::WriteAllBytes($f, $b)
function Sum($lines) { $n = 0; foreach ($x in @($lines)) { $v = 0; if ([double]::TryParse("$x".Trim(), [ref]$v)) { $n += $v } }; $n }
function Dl { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="https://speed.cloudflare.com/__down?bytes=12000000"; $r = & $curl -s -L -Z --parallel-max 4 -w "%{size_download}\n" -o NUL $u -o NUL $u -o NUL $u -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 40000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
function Ul { $sw=[Diagnostics.Stopwatch]::StartNew(); $u="https://speed.cloudflare.com/__up"; $r = & $curl -s -L -Z --parallel-max 4 -w "%{size_upload}\n" --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u --data-binary "@$f" -o NUL $u; $sw.Stop(); $by = Sum $r; if ($by -lt 38000000) { "invalid($([math]::Round($by/1e6,1))MB)" } else { [math]::Round($by*8/1e6/$sw.Elapsed.TotalSeconds,1) } }
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
