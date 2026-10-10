# USB TX aggregation test. Administrator PowerShell, dist folder se chalao. Output: Desktop\agg.txt
# Har variant: adapter reconnect -> ping gateway -> upload/download. Agar variant me ping fail ho to wo position galat hai.
param([string]$Ssid = "Airtel_Thanks")
$ProgressPreference = 'SilentlyContinue'
$o = "$env:USERPROFILE\Desktop\agg.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function St { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() }
function Dl { try { $sw=[Diagnostics.Stopwatch]::StartNew(); $r=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=30000000" -UseBasicParsing -TimeoutSec 40; $sw.Stop(); [math]::Round($r.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
function Ul { try { $b=New-Object byte[] 8000000; $sw=[Diagnostics.Stopwatch]::StartNew(); Invoke-WebRequest "http://speed.cloudflare.com/__up" -Method Post -Body $b -UseBasicParsing -TimeoutSec 40 | Out-Null; $sw.Stop(); [math]::Round(8000000*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
"== agg test $(Get-Date) ==" | Out-File $o
$variants = @( @{n=0;p=2;t="baseline (agg off)"}, @{n=4;p=2;t="agg 4, count in dword7[31:24]"}, @{n=4;p=1;t="agg 4, count in dword6[31:24]"}, @{n=4;p=0;t="agg 4, count in dword5[31:24]"} )
foreach ($v in $variants) {
  Set-ItemProperty $k -Name Cfg_TxAgg -Value $v.n -Type DWord
  Set-ItemProperty $k -Name Cfg_TxAggPos -Value $v.p -Type DWord
  Set-ItemProperty $k -Name Cfg_Ba -Value 1 -Type DWord
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  for ($i=0; $i -lt 20; $i++) { Start-Sleep 3; if ((St) -eq "connected") { break } }
  Start-Sleep 6
  $gw = (Get-NetRoute -InterfaceAlias "Wi-Fi" -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue | Select-Object -First 1).NextHop
  $png = if ($gw) { $p = Test-Connection $gw -Count 10 -ErrorAction SilentlyContinue; "$(@($p).Count)/10 replies" } else { "no gateway" }
  "--- $($v.t): state=$(St) ping=$png ---" | Tee-Object -FilePath $o -Append
  if ($png -match '^(\d+)/10' -and [int]$Matches[1] -ge 5) {
    for ($r=1; $r -le 2; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
  } else { "SKIP speed (connectivity broken)" | Tee-Object -FilePath $o -Append }
  (Get-ItemProperty $k | Select-Object Log_TxAgg_*,Log_TxAsync_*,Log_Data_TxOk,Log_Data_TxFail,Log_Data_TxDrop,Log_Rate_PickMcs7) | Format-List | Out-File $o -Append
}
Set-ItemProperty $k -Name Cfg_TxAgg -Value 0 -Type DWord
"Done -> $o  (Cfg_TxAgg wapas 0 kar diya)"
