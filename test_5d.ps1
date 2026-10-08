# Phase 5d-A test (OPEN network connect) - Administrator PowerShell me chalao, folder me jaha rtl8188eu.sys/.inf/.cat/.cer hain
# Usage:  .\test_5d.ps1 -Ssid "MeraHotspot"      (phone hotspot, security = None / Open)
param([Parameter(Mandatory=$true)][string]$Ssid)
$ErrorActionPreference = 'Continue'
$out = "$env:USERPROFILE\Desktop\diag10.txt"
"== 5d diag $(Get-Date) ssid=$Ssid ==" | Out-File $out

Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match 'rtl8188eu' } | ForEach-Object {
  pnputil /delete-driver $_.Driver /uninstall /force | Out-Null }
Import-Certificate -FilePath .\rtl8188eu-test.cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath .\rtl8188eu-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
pnputil /add-driver .\rtl8188eu.inf /install | Out-File $out -Append
Start-Sleep 15

# open profile
$hex = ($Ssid.ToCharArray() | ForEach-Object { '{0:X2}' -f [int]$_ }) -join ''
$xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
 <name>$Ssid</name>
 <SSIDConfig><SSID><hex>$hex</hex><name>$Ssid</name></SSID></SSIDConfig>
 <connectionType>ESS</connectionType><connectionMode>manual</connectionMode>
 <MSM><security><authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption></security></MSM>
</WLANProfile>
"@
$xml | Out-File "$env:TEMP\p5d.xml" -Encoding ascii
netsh wlan add profile filename="$env:TEMP\p5d.xml" | Out-File $out -Append
"--- scan (pre) ---" | Out-File $out -Append
netsh wlan show networks | Out-File $out -Append
Start-Sleep 5
"--- connect ---" | Out-File $out -Append
netsh wlan connect name="$Ssid" | Out-File $out -Append
Start-Sleep 20
netsh wlan show interfaces | Out-File $out -Append
"--- ipconfig ---" | Out-File $out -Append
ipconfig /all | Out-File $out -Append
$gw = (Get-NetRoute -DestinationPrefix 0.0.0.0/0 -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceAlias -match 'Wi-Fi' } | Select-Object -First 1).NextHop
"gateway = $gw" | Out-File $out -Append
if ($gw) { ping -n 4 $gw | Out-File $out -Append }
Start-Sleep 6

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
"--- Device Parameters ---" | Out-File $out -Append
(Get-ItemProperty $key).PSObject.Properties | Where-Object { $_.Name -match '^(Log_(Join|Wifi|TxPipe|Tx_|Data|Dp))' } |
  Sort-Object Name | ForEach-Object { "{0} = {1}" -f $_.Name, $_.Value } | Out-File $out -Append
"--- Get-NetAdapter ---" | Out-File $out -Append
Get-NetAdapter | Format-Table Name,InterfaceDescription,Status,MacAddress -AutoSize | Out-String | Out-File $out -Append
"Done -> $out"
