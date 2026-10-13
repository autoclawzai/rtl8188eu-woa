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
function Cnt { $p = Get-ItemProperty $k; "state=$(St) JoinRuns=$($p.Log_Join_Runs) KeyAdds=$($p.Log_Key_Adds) Ptk=$($p.Log_Key_Ptk) JoinStatus=$($p.Log_Join_Status) ConnDone=$($p.Log_Wifi_ConnDone) RxAdv=$($p.Log_Dp_RxAdv) TxAdv=$($p.Log_Dp_TxAdv)" }
function Events($t0) { L "--- WLAN events ---"; Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 80 -ErrorAction SilentlyContinue | Where-Object { $_.TimeCreated -ge $t0 -and $_.Id -notin 11004,11005,11010 } | Sort-Object TimeCreated | ForEach-Object { "{0}  id={1}  {2}" -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, (($_.Message -split "`r?`n" | Where-Object { $_ -match 'Reason|Failure|Result|Mode' } | Select-Object -First 3) -join ' | ') } | Out-File $o -Append }
function Phase($ls) {
  L "===== PHASE Cfg_LinkState=$ls ====="
  $t0 = Get-Date
  Set-ItemProperty $k -Name Cfg_LinkState -Value $ls -Type DWord
  Disable-NetAdapter -Name "Wi-Fi" -Confirm:$false; Start-Sleep 4; Enable-NetAdapter -Name "Wi-Fi" -Confirm:$false
  Start-Sleep 10
  $c = WaitConn 20
  if (-not $c) { netsh wlan connect name="$Ssid" | Out-Null; $c = WaitConn 12 }
  L "initial connect ok: $c   $(Cnt)"
  if (-not $c) { Events $t0; return }
  Start-Sleep 10
  L "BEFORE disconnect: $(Cnt)"
  L "netsh wlan disconnect"; netsh wlan disconnect | Out-Null; Start-Sleep 4
  L "after disconnect: $(Cnt)"
  netsh wlan connect name="$Ssid" | Out-Null
  $c2 = WaitConn 10
  L "RESULT reconnect after manual disconnect: $c2"
  for ($n = 0; $n -lt 7; $n++) { L "observe: $(Cnt)"; Start-Sleep 5 }
  L "ping: $((ping.exe -n 3 8.8.8.8 | Select-Object -Last 1))"
  Dump "final counters"
  Events $t0
}
"== diag_conn $(Get-Date) ssid=$Ssid ==" | Out-File $o
Phase 0
Phase 1
Set-ItemProperty $k -Name Cfg_LinkState -Value 0 -Type DWord
L "done (Cfg_LinkState reset to 0)"
Get-Content $o
