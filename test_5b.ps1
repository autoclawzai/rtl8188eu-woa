# Phase 5b test - Administrator PowerShell me chalao, folder me jaha rtl8188eu.sys/.inf/.cat/.cer hain
$ErrorActionPreference = 'Continue'
$out = "$env:USERPROFILE\Desktop\diag8.txt"
"== 5b diag $(Get-Date) ==" | Out-File $out

# 1) purane driver packages hatao
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match 'rtl8188eu' } | ForEach-Object {
  pnputil /delete-driver $_.Driver /uninstall /force | Out-Null }

# 2) test cert trust + naya driver
Import-Certificate -FilePath .\rtl8188eu-test.cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath .\rtl8188eu-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
pnputil /add-driver .\rtl8188eu.inf /install | Out-File $out -Append
Start-Sleep 12

# 3) device + registry logs
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
"--- PnP ---" | Out-File $out -Append
$dev | Format-List Class,FriendlyName,Status,Problem,ProblemDescription,InstanceId | Out-String | Out-File $out -Append
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
"--- Device Parameters ---" | Out-File $out -Append
(Get-ItemProperty $key).PSObject.Properties | Where-Object { $_.Name -match '^(Log_|Scan_)' } |
  Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $out -Append

# 4) Windows ko adapter dikh raha hai?
"--- Get-NetAdapter ---" | Out-File $out -Append
Get-NetAdapter -IncludeHidden | Format-Table Name,InterfaceDescription,Status,MacAddress,PhysicalMediaType -AutoSize | Out-String | Out-File $out -Append
"--- netsh wlan ---" | Out-File $out -Append
netsh wlan show drivers | Out-File $out -Append
netsh wlan show interfaces | Out-File $out -Append
netsh wlan show networks mode=bssid | Out-File $out -Append
"Done -> $out"
