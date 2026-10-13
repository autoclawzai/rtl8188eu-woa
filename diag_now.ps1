# Snapshot only (no disconnect): driver logs + IP/DHCP state + WLAN events. Output: Desktop\diag_now.txt
$o = "$env:USERPROFILE\Desktop\diag_now.txt"
$d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
"== diag_now $(Get-Date) ==" | Out-File $o
"-- state --" | Out-File $o -Append
netsh wlan show interfaces | Select-String 'State|SSID|Signal|Receive|Transmit' | Out-File $o -Append
"-- ip --" | Out-File $o -Append
Get-NetIPConfiguration -InterfaceAlias "Wi-Fi" -ErrorAction SilentlyContinue | Out-String | Out-File $o -Append
Get-NetIPAddress -InterfaceAlias "Wi-Fi" -ErrorAction SilentlyContinue | Select-Object IPAddress,PrefixOrigin,AddressState | Format-Table | Out-String | Out-File $o -Append
"-- LinkState cfg --" | Out-File $o -Append
"Cfg_LinkState = $((Get-ItemProperty $k).Cfg_LinkState)" | Out-File $o -Append
"-- driver logs --" | Out-File $o -Append
(Get-ItemProperty $k).PSObject.Properties | Where-Object { $_.Name -match '^Log_(Dp_|Key_|Join_|Data_(Rx|Tx)|Wifi_(ConnDone|LinkLost)|Dbg_Tx(All|Cls))' } | Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $o -Append
"-- WLAN events --" | Out-File $o -Append
Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 30 -ErrorAction SilentlyContinue | Where-Object { $_.Id -notin 11004,11005,11010 } | Sort-Object TimeCreated | ForEach-Object { "{0}  id={1}  {2}" -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, (($_.Message -split "`r?`n" | Where-Object { $_ -match 'Reason|Failure|Mode' } | Select-Object -First 3) -join ' | ') } | Out-File $o -Append
"-- DHCP client events --" | Out-File $o -Append
Get-WinEvent -LogName 'Microsoft-Windows-Dhcp-Client/Admin' -MaxEvents 15 -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object { "{0}  id={1}  {2}" -f $_.TimeCreated.ToString('HH:mm:ss'), $_.Id, ($_.Message -split "`r?`n")[0] } | Out-File $o -Append
Get-Content $o
