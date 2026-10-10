# Reconnect test - Administrator PowerShell me chalao.
# Step 1: script chalate hi adapter disable/enable hota hai -> Windows ko khud (Automatic) connect karna chahiye.
# Step 2: jab script "ROUTER OFF KARO" bole -> router ka power band karo, 20 sec baad on karo.
# Script 4 minute tak har 10 sec state log karta hai -> Desktop\reconnect2.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\reconnect2.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function St { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() }
function Stamp { Get-Date -Format "HH:mm:ss" }

netsh wlan set profileparameter name="$Ssid" connectionmode=auto | Out-Null
"== reconnect test $(Get-Date) ssid=$Ssid ==" | Out-File $o

"[$(Stamp)] adapter disable/enable (Windows ko Automatic connect karna chahiye)" | Tee-Object -FilePath $o -Append
Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false
Start-Sleep 5
Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
$connected = $false
for ($i = 0; $i -lt 18; $i++) {
  Start-Sleep 5
  $s = St
  "[$(Stamp)] state=$s" | Out-File $o -Append
  if ($s -eq "connected") { $connected = $true; break }
}
"[$(Stamp)] auto connect after enable: $connected" | Tee-Object -FilePath $o -Append
if (-not $connected) { "Auto connect nahi hua, aage ka test nahi ho sakta. reconnect2.txt bhej do." | Tee-Object -FilePath $o -Append; return }

Write-Host ""
Write-Host ">>> ROUTER OFF KARO (power band), 20 sec baad ON karo. Script 4 minute tak dekhega. <<<" -ForegroundColor Yellow
"[$(Stamp)] watching 4 min (router off/on karo)" | Out-File $o -Append
$prev = ""
for ($i = 0; $i -lt 24; $i++) {
  $s = St
  $ln = (Get-ItemProperty $k)
  "[$(Stamp)] state=$s  LinkLost=$($ln.Log_Wifi_LinkLost) DisassocGen=$($ln.Log_Wifi_DisassocGen) ScanTasks=$($ln.Log_Wifi_ScanTasks) JoinRuns=$($ln.Log_Join_Runs) ConnSecure=$($ln.Log_Wifi_ConnSecure) ConnBody=$($ln.Log_Wifi_ConnBodyLen)" | Out-File $o -Append
  Start-Sleep 10
}
"== WLAN events ==" | Out-File $o -Append
Get-WinEvent -LogName "Microsoft-Windows-WLAN-AutoConfig/Operational" -MaxEvents 40 -ErrorAction SilentlyContinue |
  Where-Object { $_.Id -in 8000,8001,8002,8003,11000,11001 } | ForEach-Object {
    $m = ($_.Message -split "`r?`n" | Where-Object { $_ -match "Connection Mode|Authentication|Encryption|Reason|PHY" }) -join " | "
    "{0} id={1} {2}" -f $_.TimeCreated.ToString("HH:mm:ss"), $_.Id, $m } | Out-File $o -Append
"Done -> $o"
