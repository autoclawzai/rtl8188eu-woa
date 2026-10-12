# QoS-data TX test (upload gap). Administrator PowerShell, dist folder se chalao. Output: Desktop\qos.txt
# 6 variants: har ek me adapter reconnect -> ping gateway -> speed (2 round). Agar ping fail ho to variant skip hota hai.
param([string]$Ssid = "Airtel_Thanks")
$ProgressPreference = 'SilentlyContinue'
$o = "$env:USERPROFILE\Desktop\qos.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function St { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() }
function Dl { try { $sw=[Diagnostics.Stopwatch]::StartNew(); $r=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=30000000" -UseBasicParsing -TimeoutSec 40; $sw.Stop(); [math]::Round($r.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
function Ul { try { $b=New-Object byte[] 8000000; $sw=[Diagnostics.Stopwatch]::StartNew(); Invoke-WebRequest "http://speed.cloudflare.com/__up" -Method Post -Body $b -UseBasicParsing -TimeoutSec 40 | Out-Null; $sw.Stop(); [math]::Round(8000000*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
"== qos test $(Get-Date) ==" | Out-File $o
$variants = @( @{q=0;t="QoS convert OFF (purana)"}, @{q=1;t="QoS convert ON (real driver jaisa QoS data)"} )
foreach ($v in $variants) {
  Set-ItemProperty $k -Name Cfg_QosTx -Value $v.q -Type DWord
    Set-ItemProperty $k -Name Cfg_TxAgg -Value 0 -Type DWord
  Set-ItemProperty $k -Name Cfg_Ba -Value 1 -Type DWord
  netsh wlan set profileparameter name="$Ssid" connectionmode=auto | Out-Null
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  for ($i=0; $i -lt 20; $i++) { Start-Sleep 3; if ((St) -eq "connected") { break } }
  Start-Sleep 6
  $gw = (Get-NetRoute -InterfaceAlias "Wi-Fi" -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue | Select-Object -First 1).NextHop
  $png = if ($gw) { $p = Test-Connection $gw -Count 10 -ErrorAction SilentlyContinue; "$(@($p).Count)/10 replies" } else { "no gateway" }
  $ls = (Get-NetAdapter -Name "Wi-Fi" -ErrorAction SilentlyContinue).LinkSpeed
  "--- $($v.t): state=$(St) ping=$png linkspeed=$ls ---" | Tee-Object -FilePath $o -Append
  if ($png -match '^(\d+)/10' -and [int]$Matches[1] -ge 5) {
    for ($r=1; $r -le 3; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
  } else { "SKIP speed (connectivity broken)" | Tee-Object -FilePath $o -Append }
  (Get-ItemProperty $k | Select-Object Log_Ht40_ApSec,Log_Ht40_Status,Log_Join_Status,Log_Join_AuthTries,Log_Join_AssocTries,Log_Ba_Req,Log_Ba_Resp,Log_Data_TxOk,Log_Data_TxFail,Log_Rate_PickMcs7) | Format-List | Out-File $o -Append
}
Set-ItemProperty $k -Name Cfg_QosTx -Value 1 -Type DWord
"Done -> $o  (default: QosTx=1)"
