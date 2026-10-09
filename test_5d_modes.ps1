# Phase 5d TX-mode experiment. Driver pehle se install ho (test_5d.ps1 chala chuka ho). Admin PowerShell me chalao.
# Usage:  .\test_5d_modes.ps1 -Ssid "Asteroid"
param([Parameter(Mandatory=$true)][string]$Ssid)
$out = "$env:USERPROFILE\Desktop\diag11.txt"
"== 5d TX modes $(Get-Date) ssid=$Ssid ==" | Out-File $out
$id = (Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1).InstanceId
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
function Dump($tag) {
  $p = Get-ItemProperty $key
  $line = "{0}: TxMode={1} TxOk={2} TxFail={3} Rx(data)={4} Type2={5} FltNoData={6} RxFrames={7}" -f $tag,$p.Log_TxMode_Active,$p.Log_Data_TxOk,$p.Log_Data_TxFail,$p.Log_Data_Rx,$p.Log_Rx_Type2,$p.Log_Rx_FltNoData,$p.Log_Data_RxFrames
  $line | Out-File $out -Append; $line
}
netsh wlan connect name="$Ssid" | Out-File $out -Append
Start-Sleep 8
netsh wlan show interfaces | Select-String 'State|SSID|BSSID|Channel' | Out-File $out -Append
foreach ($m in 0,1,2,3,4,5,6,7) {
  Set-ItemProperty -Path $key -Name Cfg_TxMode -Value $m -Type DWord
  Start-Sleep 5
  $before = (Get-ItemProperty $key).Log_Data_Rx
  $pr = Start-Process -FilePath ipconfig -ArgumentList '/renew','"Wi-Fi"' -PassThru -WindowStyle Hidden
  if (-not $pr.WaitForExit(20000)) { $pr.Kill() }
  Start-Sleep 4
  Dump "mode $m"
  $ip = (Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
  "   IPv4 = $ip" | Out-File $out -Append
  if ($ip -and $ip -notmatch '^169\.254') { "   >>> DHCP chal gaya in mode $m" | Out-File $out -Append; break }
}
"--- final counters ---" | Out-File $out -Append
(Get-ItemProperty $key).PSObject.Properties | Where-Object { $_.Name -match '^Log_(Data|Dp|Rx_(Type2|Flt|First)|TxMode)' } | Sort-Object Name |
  ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $out -Append
"Done -> $out"
