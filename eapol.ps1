# EAPOL handshake timeline (from the driver) merged with Windows WLAN/DHCP/profile events. Output: Desktop\eapol.txt
param([int]$Minutes = 6)
$o = "$env:USERPROFILE\Desktop\eapol.txt"
$now = Get-Date
$t0 = $now.AddMinutes(-$Minutes)
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
$p = Get-ItemProperty $k
$bootMs = ((Get-CimInstance Win32_OperatingSystem).LastBootUpTime - $now).TotalMilliseconds * -1
function Decode($ki) {
  if ($ki -eq 0xFFFF) { return "EAP-other" }
  $pw = ($ki -band 0x08) -ne 0; $ins = ($ki -band 0x40) -ne 0; $ack = ($ki -band 0x80) -ne 0; $mic = ($ki -band 0x100) -ne 0; $sec = ($ki -band 0x200) -ne 0
  if ($pw) { if ($ack -and -not $mic) { "4W-msg1" } elseif ($ack -and $mic) { "4W-msg3" } elseif ($mic -and -not $sec) { "4W-msg2" } else { "4W-msg4" } }
  else { if ($ack) { "GRP-msg1" } else { "GRP-msg2" } }
}
$rows = @()
$n = [int]$p.Log_Eapol_N
for ($q = 0; $q -lt [Math]::Min($n, 24); $q++) {
  $t = $p.("Log_Eapol_T{0:00}" -f $q); $i = [uint32]$p.("Log_Eapol_I{0:00}" -f $q)
  if (-not $t) { continue }
  $wall = $now.AddMilliseconds(-($bootMs - $t))
  $dir = if ($i -band 0x80000000) { "RX" } else { "TX" }
  $ki = ($i -shr 8) -band 0xFFFF; $rc = $i -band 0xFF
  $rows += [pscustomobject]@{ T = $wall; Src = "DRV"; Id = $dir; Msg = ("{0} keyinfo=0x{1:X4} rc={2}" -f (Decode $ki), $ki, $rc) }
}
$logs = @{ 'WLAN' = 'Microsoft-Windows-WLAN-AutoConfig/Operational'; 'DHCP' = 'Microsoft-Windows-Dhcp-Client/Admin'; 'PROF' = 'Microsoft-Windows-NetworkProfile/Operational' }
foreach ($nme in $logs.Keys) {
  Get-WinEvent -FilterHashtable @{ LogName = $logs[$nme]; StartTime = $t0 } -ErrorAction SilentlyContinue | ForEach-Object {
    if ($nme -eq 'DHCP' -and $_.Id -in 50065, 50067) { return }
    $m = ($_.Message -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if ($m.Length -gt 100) { $m = $m.Substring(0, 100) }
    $rows += [pscustomobject]@{ T = $_.TimeCreated; Src = $nme; Id = $_.Id; Msg = $m }
  }
}
"== eapol timeline $now (driver EAPOL count=$n; JoinRuns=$($p.Log_Join_Runs); KeyAdds=$($p.Log_Key_Adds)) ==" | Out-File $o
$rows | Where-Object { $_.T -ge $t0 } | Sort-Object T | ForEach-Object { "{0}  {1,-4} {2,-6} {3}" -f $_.T.ToString('HH:mm:ss.fff'), $_.Src, $_.Id, $_.Msg } | Out-File $o -Append
Get-Content $o
