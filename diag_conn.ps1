# Connect-after-disconnect repro, 2 phases (Cfg_LinkState 0 then 1). Administrator PowerShell. Output: Desktop\diag_conn.txt
param([string]$Ssid = "Airtel_Thanks")
$o = "$env:USERPROFILE\Desktop\diag_conn.txt"
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
function St { try { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() } catch { "?" } }
function Stamp { Get-Date -Format "HH:mm:ss" }
function L($m) { "[$(Stamp)] $m" | Out-File $o -Append }
function Dump($t) { L "--- $t ---"; (Get-ItemProperty $k).PSObject.Properties | Where-Object { $_.Name -match '^Log_(Dp_|Key_Adds|Key_Dels|Key_Ptk|Join_(Runs|Status|AuthStatus|AssocStatus|HwUp)|Data_Rx$|Wifi_(LinkLost|ConnDone))' } | Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $o -Append }
function WaitConn($n) { for ($i = 0; $i -lt $n; $i++) { Start-Sleep 3; if ((St) -eq "connected") { return $true } }; return $false }
function Phase($ls) {
  L "===== PHASE Cfg_LinkState=$ls ====="
  Set-ItemProperty $k -Name Cfg_LinkState -Value $ls -Type DWord
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  netsh wlan connect name="$Ssid" | Out-Null
  $c = WaitConn 12
  L "initial connect ok: $c"
  if (-not $c) { Dump "after failed initial connect"; return }
  Start-Sleep 5
  Dump "BEFORE disconnect"
  L "netsh wlan disconnect"; netsh wlan disconnect | Out-Null; Start-Sleep 4
  L "state after disconnect: $(St)"
  netsh wlan connect name="$Ssid" | Out-Null
  $c2 = WaitConn 10
  L "RESULT reconnect after manual disconnect: $c2"
  Dump "AFTER reconnect attempt"
  if ($c2) { L "ping test"; (ping.exe -n 3 8.8.8.8 | Select-Object -Last 2) | Out-File $o -Append }
}
"== diag_conn $(Get-Date) ssid=$Ssid ==" | Out-File $o
Phase 0
Phase 1
Set-ItemProperty $k -Name Cfg_LinkState -Value 0 -Type DWord
L "done (Cfg_LinkState reset to 0)"
Get-Content $o
