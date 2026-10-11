# x86 PC par Realtek driver ka USB capture (USBPcap). Administrator PowerShell me chalao:
#   powershell -ExecutionPolicy Bypass -File .\capture_x86.ps1
# Pehli baar: USBPcap install karega -> PC RESTART karo -> script dobara chalao.
# Dusri baar: capture lega -> Desktop\usb_capture.zip banayega -> wo zip mujhe bhej do.
param([string]$CmdPath = "")
$ErrorActionPreference = "Continue"
$ProgressPreference = 'SilentlyContinue'
$desk = [Environment]::GetFolderPath("Desktop")
$work = Join-Path $desk "usbcap"
New-Item -ItemType Directory -Force $work | Out-Null
$log = Join-Path $work "capture_log.txt"
function L($m) { $t = "[$(Get-Date -Format HH:mm:ss)] $m"; Write-Host $t; $t | Out-File $log -Append }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Host "Administrator PowerShell chahiye." -ForegroundColor Red; return }

# ---------- 1. USBPcap install ----------
$cands = @($CmdPath, "$env:ProgramFiles\USBPcap\USBPcapCMD.exe", "${env:ProgramFiles(x86)}\USBPcap\USBPcapCMD.exe", "C:\Program Files\USBPcap\USBPcapCMD.exe")
$cmd = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $cmd) { $g = Get-Command USBPcapCMD.exe -ErrorAction SilentlyContinue; if ($g) { $cmd = $g.Source } }
if (-not $cmd) {   # registry uninstall entry / folder search
  $reg = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*" -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like "*USBPcap*" } | Select-Object -First 1
  if ($reg -and $reg.InstallLocation) { $c = Join-Path $reg.InstallLocation "USBPcapCMD.exe"; if (Test-Path $c) { $cmd = $c } }
}
if (-not $cmd) { $f = Get-ChildItem "$env:ProgramFiles", "${env:ProgramFiles(x86)}", "$env:USERPROFILE\Downloads" -Recurse -Filter USBPcapCMD.exe -ErrorAction SilentlyContinue | Select-Object -First 1; if ($f) { $cmd = $f.FullName } }
if (-not $cmd) {
  L "USBPcap nahi mila, download kar raha hu (GitHub releases)"
  try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $rel = Invoke-RestMethod "https://api.github.com/repos/desowin/usbpcap/releases/latest" -Headers @{ "User-Agent" = "ps" }
    $asset = $rel.assets | Where-Object { $_.name -like "USBPcapSetup*.exe" } | Select-Object -First 1
    $inst = Join-Path $work $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $inst -UseBasicParsing
    L "installer: $inst, install chal raha hai"
    Start-Process $inst -ArgumentList "/S" -Wait
  } catch { L "Auto-download fail: $_"; Write-Host "Manual: https://desowin.org/usbpcap/ se USBPcap install karo, restart karo, script dobara chalao." -ForegroundColor Yellow; return }
  Write-Host ""
  Write-Host ">>> USBPcap install ho gaya. Ab PC RESTART karo, phir ye script DOBARA chalao. <<<" -ForegroundColor Yellow
  return
}
L "USBPcapCMD: $cmd"

# ---------- helpers ----------
function Start-Cap([string]$tag) {
  # Har USB root hub (USBPcap1..USBPcap10) par capture. Jo hub nahi hai wo process turant exit ho jata hai.
  $procs = @()
  foreach ($i in 1..10) {
    $out = Join-Path $work "${tag}_hub$i.pcap"
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = $null
    foreach ($flags in @("-A --capture-from-new-devices", "-A")) {
      $args = "-d \\.\USBPcap$i -o `"$out`" -s 256 $flags"
      $p = Start-Process $cmd -ArgumentList $args -PassThru -WindowStyle Hidden
      Start-Sleep -Milliseconds 700
      if (-not $p.HasExited) { break }
    }
    if ($p -and -not $p.HasExited) { $procs += $p; L "capture chal raha: USBPcap$i -> $out" }
  }
  if ($procs.Count -eq 0) { L "Koi capture start nahi hua (restart hua tha?)" }
  return ,$procs
}
function Stop-Cap($procs) { foreach ($p in $procs) { try { if (-not $p.HasExited) { Stop-Process $p -Force } } catch {} }; Start-Sleep 1 }
function WifiState { try { ((netsh wlan show interfaces | Select-String '^\s+State').ToString() -split ':')[1].Trim() } catch { "?" } }

$summary = Join-Path $work "summary.txt"
"== x86 capture $(Get-Date) ==" | Out-File $summary
(Get-NetAdapter | Where-Object { $_.InterfaceDescription -match "Realtek|802.11|Wireless" } | Format-List Name,InterfaceDescription,DriverVersion,DriverDate,Status,LinkSpeed | Out-String) | Out-File $summary -Append
(Get-PnpDevice | Where-Object { $_.InstanceId -match "VID_2357" } | Format-List FriendlyName,InstanceId,Status | Out-String) | Out-File $summary -Append

# ---------- 2. Capture A: plug-in + connect ----------
Write-Host ""
Write-Host "=== CAPTURE A (connect) ===" -ForegroundColor Cyan
Read-Host "TL-WN722N adapter USB se NIKAAL do, phir Enter dabao"
$capA = Start-Cap "A_connect"
Write-Host ">>> Ab adapter wapas LAGAO. Driver load hoga aur Wi-Fi apne aap connect hona chahiye (saved network)." -ForegroundColor Yellow
$ok = $false
for ($i = 0; $i -lt 40; $i++) { Start-Sleep 3; if ((WifiState) -eq "connected") { $ok = $true; break } }
L "wifi connected after plug: $ok"
if (-not $ok) { Write-Host "Auto-connect nahi hua - Windows se manually connect kar do, phir yahan Enter dabao." -ForegroundColor Yellow; Read-Host "Connect hone ke baad Enter" | Out-Null }
Start-Sleep 12
Stop-Cap $capA
netsh wlan show interfaces | Out-File $summary -Append

# ---------- 3. Capture B: speed test ----------
Write-Host ""
Write-Host "=== CAPTURE B (speed) ===" -ForegroundColor Cyan
$capB = Start-Cap "B_speed"
Start-Sleep 2
function Dl { try { $sw=[Diagnostics.Stopwatch]::StartNew(); $r=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=40000000" -UseBasicParsing -TimeoutSec 60; $sw.Stop(); [math]::Round($r.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
function Ul { try { $b=New-Object byte[] 10000000; $sw=[Diagnostics.Stopwatch]::StartNew(); Invoke-WebRequest "http://speed.cloudflare.com/__up" -Method Post -Body $b -UseBasicParsing -TimeoutSec 60 | Out-Null; $sw.Stop(); [math]::Round(10000000*8/1e6/$sw.Elapsed.TotalSeconds,1) } catch { "err" } }
$d = Dl; L "download Mbps: $d"
$u = Ul; L "upload Mbps: $u"
"speed: down=$d Mbps up=$u Mbps" | Out-File $summary -Append
Start-Sleep 2
Stop-Cap $capB

# ---------- 4. Zip ----------
Get-ChildItem $work -Filter *.pcap | Where-Object { $_.Length -lt 200 } | Remove-Item -ErrorAction SilentlyContinue
$zip = Join-Path $desk "usb_capture.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
$files = Get-ChildItem $work -File | Where-Object { $_.Extension -in ".pcap", ".txt" }
Compress-Archive -Path $files.FullName -DestinationPath $zip -Force
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
L "Done: $zip ($mb MB)"
Write-Host ""
Write-Host "Done -> $zip  ($mb MB). Ye zip mujhe bhej do." -ForegroundColor Green
if ($mb -gt 28) { Write-Host "Zip 28 MB se bada hai - batao, main chhota capture ka tareeka dunga." -ForegroundColor Yellow }
