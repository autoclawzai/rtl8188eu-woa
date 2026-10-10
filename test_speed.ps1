# Speed A/B test (Cfg_Ba=1 vs 0). Administrator PowerShell, dist folder se chalao. Output: Desktop\speed.txt
param([int]$Rounds = 3, [string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\speed.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function St { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() }
function Dl  { $sw=[Diagnostics.Stopwatch]::StartNew(); $r=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=30000000" -UseBasicParsing -TimeoutSec 60; $sw.Stop(); [math]::Round($r.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) }
function Ul  { $b=New-Object byte[] 8000000; $sw=[Diagnostics.Stopwatch]::StartNew(); Invoke-WebRequest "http://speed.cloudflare.com/__up" -Method Post -Body $b -UseBasicParsing -TimeoutSec 60 | Out-Null; $sw.Stop(); [math]::Round(8000000*8/1e6/$sw.Elapsed.TotalSeconds,1) }
"== speed test $(Get-Date) ==" | Out-File $o
foreach ($ba in 1,0) {
  Set-ItemProperty $k -Name Cfg_Ba -Value $ba -Type DWord
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  for ($i=0; $i -lt 20; $i++) { Start-Sleep 3; if ((St) -eq "connected") { break } }
  Start-Sleep 5
  "--- Cfg_Ba=$ba state=$(St) ---" | Tee-Object -FilePath $o -Append
  for ($r=1; $r -le $Rounds; $r++) {
    try { $d = Dl } catch { $d = "err" }
    try { $u = Ul } catch { $u = "err" }
    "round $r  down=$d Mbps  up=$u Mbps" | Tee-Object -FilePath $o -Append
  }
  (Get-ItemProperty $k | Select-Object Log_Ba_Req,Log_Ba_Resp,Log_Rate_*,Log_Data_Tx*,Log_Data_Rx*,Log_Sec_*,Log_Tx_*) | Format-List | Out-File $o -Append
}
"Done -> $o"
