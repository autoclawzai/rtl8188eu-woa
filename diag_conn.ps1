# Connect-failure diagnostics: run right AFTER a failed connect. Output: Desktop\diag_conn.txt (also printed)
$o = "$env:USERPROFILE\Desktop\diag_conn.txt"
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
"== diag_conn $(Get-Date) ==" | Out-File $o
"-- adapter --" | Out-File $o -Append
Get-NetAdapter | Where-Object { $_.InterfaceDescription -match 'RTL8188' } | Select-Object Name,Status,LinkSpeed | Format-List | Out-File $o -Append
netsh wlan show interfaces | Out-File $o -Append
"-- driver logs --" | Out-File $o -Append
$p = Get-ItemProperty $k
$p.PSObject.Properties | Where-Object { $_.Name -match '^Log_(Join|Wifi|Scan)' } | Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $o -Append
"-- WLAN events (newest first) --" | Out-File $o -Append
Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 40 | Where-Object { $_.Id -notin 11004,11005,11010 } | ForEach-Object {
  "{0}  id={1}  {2}" -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, (($_.Message -split "`r?`n" | Where-Object { $_ -match 'Reason|Connection|Failure|failed|Profile|disconnect|Result|Authentication|Encryption' } | Select-Object -First 5) -join ' | ')
} | Out-File $o -Append
Get-Content $o
