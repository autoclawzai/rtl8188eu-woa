# One-shot: latest build + test scripts download -> Downloads\rtl8188eu, driver install, test run.
# Usage (any PowerShell):
#   iwr -useb https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/main/update.ps1 -OutFile "$env:USERPROFILE\Downloads\update.ps1"; powershell -ExecutionPolicy Bypass -File "$env:USERPROFILE\Downloads\update.ps1"
# Optional: -Test pipe|speed|qos|ht40|dlcpu|agg|reconnect|none   -Ssid "Airtel_Thanks"
param([string]$Test = "pipe", [string]$Ssid = "Airtel_Thanks")
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# admin needed (driver install) -> relaunch elevated
$adm = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $adm) {
  Write-Host "Admin rights chahiye, dobara admin me khol raha hu..." -ForegroundColor Yellow
  Start-Process powershell -Verb RunAs -ArgumentList "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Test $Test -Ssid `"$Ssid`""
  return
}

$repo = "autoclawzai/rtl8188eu-woa"
$root = Join-Path $env:USERPROFILE "Downloads\rtl8188eu"
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force $root | Out-Null
Write-Host "== 1/5 download ==" -ForegroundColor Cyan
$zip = Join-Path $root "rtl8188eu-dist.zip"
Invoke-WebRequest "https://github.com/$repo/releases/download/latest/rtl8188eu-dist.zip" -OutFile $zip -UseBasicParsing
foreach ($f in "test_pipe","test_speed","test_qos","test_ht40","test_dlcpu","test_agg","test_reconnect") {
  Invoke-WebRequest "https://raw.githubusercontent.com/$repo/main/$f.ps1" -OutFile (Join-Path $root "$f.ps1") -UseBasicParsing
}
Write-Host "== 2/5 extract ==" -ForegroundColor Cyan
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
Expand-Archive $zip $dist -Force
Get-ChildItem $dist | ForEach-Object { Write-Host ("  {0} {1}" -f $_.Name, $_.Length) }

if (-not ((bcdedit /enum) -match 'testsigning\s+Yes')) { Write-Host "WARNING: test signing ON nahi hai. 'bcdedit /set testsigning on' karke reboot karo." -ForegroundColor Red }

Write-Host "== 3/5 purana driver hatao + certificate ==" -ForegroundColor Cyan
# driver uninstall interface delete karta hai -> Windows WiFi profiles (password) bhi chali jaati hain. Pehle backup, baad me restore.
$prof = Join-Path $root "profiles"
if (Test-Path $prof) { Remove-Item $prof -Recurse -Force }
New-Item -ItemType Directory -Force $prof | Out-Null
foreach ($l in (netsh wlan show profiles)) {
  if ($l -match 'All User Profile\s*:\s*(.+)$' -or $l -match 'Profile\s*:\s*(.+)$') { $pn = $Matches[1].Trim(); netsh wlan export profile name="$pn" key=clear folder="$prof" | Out-Null }
}
Write-Host ("  {0} WiFi profile backup" -f @(Get-ChildItem $prof -Filter *.xml).Count)
$cur = $null
foreach ($l in (pnputil /enum-drivers)) {
  if ($l -match 'Published Name\s*:\s*(oem\d+\.inf)') { $cur = $Matches[1] }
  if ($l -match 'Original Name\s*:\s*rtl8188eu\.inf' -and $cur) { Write-Host "  delete $cur"; pnputil /delete-driver $cur /uninstall /force | Out-Null }
}
$cer = Join-Path $dist "rtl8188eu-test.cer"
certutil -addstore Root $cer | Out-Null
certutil -addstore TrustedPublisher $cer | Out-Null

Write-Host "== 4/5 driver install ==" -ForegroundColor Cyan
pnputil /add-driver (Join-Path $dist "rtl8188eu.inf") /install | Out-Host
pnputil /scan-devices | Out-Null
$ok = $false
for ($i = 0; $i -lt 30; $i++) {
  Start-Sleep 2
  $d = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
  if ($d -and $d.Status -eq 'OK') { $ok = $true; break }
}
if (-not $ok) { Write-Host "Device OK state me nahi aaya. Adapter nikaal ke dobara lagao, phir script chalao." -ForegroundColor Red; return }
Write-Host "  adapter OK. WiFi connect ka wait..."
foreach ($x in (Get-ChildItem $prof -Filter *.xml -ErrorAction SilentlyContinue)) { netsh wlan add profile filename="$($x.FullName)" | Out-Null }
Remove-Item $prof -Recurse -Force -ErrorAction SilentlyContinue      # clear-text password files hata do
netsh wlan connect name="$Ssid" | Out-Null
netsh wlan set profileparameter name="$Ssid" connectionmode=auto 2>$null | Out-Null
for ($i = 0; $i -lt 25; $i++) {
  Start-Sleep 3
  $s = (netsh wlan show interfaces | Select-String '^\s+State').ToString()
  if ($s -match 'connected' -and $s -notmatch 'disconnected') { break }
}
Start-Sleep 5

Write-Host "== 5/5 test: $Test ==" -ForegroundColor Cyan
if ($Test -ne "none") {
  $t = Join-Path $root "test_$Test.ps1"
  if (Test-Path $t) { Push-Location $root; & $t -Ssid $Ssid; Pop-Location } else { Write-Host "test_$Test.ps1 nahi mila" -ForegroundColor Red }
}
Write-Host "Done. Result txt Desktop pe hai (pipe/speed/qos/ht40/dlcpu .txt)." -ForegroundColor Green
