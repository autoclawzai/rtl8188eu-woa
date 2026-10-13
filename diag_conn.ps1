# Connect-after-disconnect repro + diagnostics. Run while Wi-Fi is CONNECTED (Administrator). Output: Desktop\diag_conn.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\diag_conn.txt"
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
function St { try { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() } catch { "?" } }
function Stamp { Get-Date -Format "HH:mm:ss" }
function Dump($title) {
  "--- $title ---" | Out-File $o -Append
  $p = Get-ItemProperty $k
  $p.PSObject.Properties | Where-Object { $_.Name -match '^Log_' } | Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $o -Append
}
"== diag_conn $(Get-Date) ssid=$Ssid ==" | Out-File $o
"[$(Stamp)] start state=$(St)" | Out-File $o -Append
Dump "driver logs BEFORE"
$t0 = Get-Date
"[$(Stamp)] netsh wlan disconnect" | Out-File $o -Append
netsh wlan disconnect | Out-Null
Start-Sleep 4
"[$(Stamp)] after disconnect state=$(St)" | Out-File $o -Append
"[$(Stamp)] netsh wlan connect" | Out-File $o -Append
netsh wlan connect name="$Ssid" | Out-File $o -Append
$ok = $false
for ($i = 0; $i -lt 12; $i++) { Start-Sleep 3; $s = St; "[$(Stamp)] state=$s" | Out-File $o -Append; if ($s -eq "connected") { $ok = $true; break } }
"[$(Stamp)] RESULT reconnect after manual disconnect: $ok" | Out-File $o -Append
Dump "driver logs AFTER"
"--- WLAN events since start ---" | Out-File $o -Append
Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 60 | Where-Object { $_.TimeCreated -ge $t0 -and $_.Id -notin 11004,11005,11010 } | Sort-Object TimeCreated | ForEach-Object {
  "{0}  id={1}  {2}" -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, (($_.Message -split "`r?`n" | Where-Object { $_ -match 'Reason|Failure|Result|disconnect' } | Select-Object -First 3) -join ' | ')
} | Out-File $o -Append
Get-Content $o
