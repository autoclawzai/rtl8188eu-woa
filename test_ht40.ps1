# HT40 + TX descriptor bit test. Administrator PowerShell, dist folder se chalao. Output: Desktop\ht40.txt
# 6 variants: har ek me adapter reconnect -> ping gateway -> speed (2 round). Agar ping fail ho to variant skip hota hai.
param([string]$Ssid = "Airtel_Thanks")
$ProgressPreference = 'SilentlyContinue'
$o = "$env:USERPROFILE\Desktop\ht40.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function St { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() }
function Dl { try { $sw=[Diagnostics.Stopwatch]::StartNew(); $r=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=30000000" -UseBasicParsing -TimeoutSec 40; $sw.Stop(); [math]::Round($r.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
function Ul { try { $b=New-Object byte[] 8000000; $sw=[Diagnostics.Stopwatch]::StartNew(); Invoke-WebRequest "http://speed.cloudflare.com/__up" -Method Post -Body $b -UseBasicParsing -TimeoutSec 40 | Out-Null; $sw.Stop(); [math]::Round(8000000*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
"== ht40 test $(Get-Date) ==" | Out-File $o
$variants = @(
  @{h=1;m=12;t="HT40 + BW40 bit + SGI (mask 12)"},
  @{h=1;m=28;t="mask 12 + fallback d6 (28)"},
  @{h=1;m=13;t="mask 12 + 16B pad (13)"},
  @{h=1;m=14;t="mask 12 + AGG_EN (14)"},
  @{h=1;m=44;t="mask 12 + CTS-self d4 (44)"},
  @{h=0;m=8;t="20MHz + SGI only (8)"} )
foreach ($v in $variants) {
  Set-ItemProperty $k -Name Cfg_Ht40 -Value $v.h -Type DWord
  Set-ItemProperty $k -Name Cfg_DescMask -Value $v.m -Type DWord
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
    for ($r=1; $r -le 2; $r++) { "round $r down=$(Dl) Mbps up=$(Ul) Mbps" | Tee-Object -FilePath $o -Append }
  } else { "SKIP speed (connectivity broken)" | Tee-Object -FilePath $o -Append }
  (Get-ItemProperty $k | Select-Object Log_Ht40_ApSec,Log_Ht40_Status,Log_Join_Status,Log_Join_AuthTries,Log_Join_AssocTries,Log_Ba_Req,Log_Ba_Resp,Log_Data_TxOk,Log_Data_TxFail,Log_Rate_PickMcs7) | Format-List | Out-File $o -Append
}
Set-ItemProperty $k -Name Cfg_Ht40 -Value 1 -Type DWord
Set-ItemProperty $k -Name Cfg_DescMask -Value 12 -Type DWord
"Done -> $o  (defaults: Ht40=1, DescMask=12)"
