# Merged timeline (WLAN + DHCP + network profile + NLA) of the last N minutes. Run right after a slow connect. Output: Desktop\timeline.txt
param([int]$Minutes = 8)
$o = "$env:USERPROFILE\Desktop\timeline.txt"
$t0 = (Get-Date).AddMinutes(-$Minutes)
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
$p = Get-ItemProperty $k
"== timeline $(Get-Date)  last $Minutes min ==" | Out-File $o
"driver: JoinRuns=$($p.Log_Join_Runs) KeyAdds=$($p.Log_Key_Adds) JoinStatus=$($p.Log_Join_Status) AuthTries=$($p.Log_Join_AuthTries) AssocTries=$($p.Log_Join_AssocTries) TxSubmitFail=$($p.Log_Dp_TxSubmitFail) LastTxSt=$($p.Log_Dp_TxLastSt)" | Out-File $o -Append
"state: $((netsh wlan show interfaces | Select-String '^\s+State').ToString().Trim())" | Out-File $o -Append
$logs = @{
  'WLAN' = 'Microsoft-Windows-WLAN-AutoConfig/Operational';
  'DHCP' = 'Microsoft-Windows-Dhcp-Client/Admin';
  'NLA'  = 'Microsoft-Windows-NlaSvc/Operational';
  'PROF' = 'Microsoft-Windows-NetworkProfile/Operational'
}
$all = foreach ($n in $logs.Keys) {
  Get-WinEvent -FilterHashtable @{ LogName = $logs[$n]; StartTime = $t0 } -ErrorAction SilentlyContinue | ForEach-Object {
    $m = ($_.Message -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 2) -join ' | '
    if ($m.Length -gt 170) { $m = $m.Substring(0, 170) }
    [pscustomobject]@{ T = $_.TimeCreated; Src = $n; Id = $_.Id; Msg = $m }
  }
}
$all | Sort-Object T | ForEach-Object { "{0}  {1,-4} {2,-6} {3}" -f $_.T.ToString('HH:mm:ss.fff'), $_.Src, $_.Id, $_.Msg } | Out-File $o -Append
Get-Content $o
