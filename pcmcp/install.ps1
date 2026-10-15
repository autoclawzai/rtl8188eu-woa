# pcmcp installer: MCP server + ngrok tunnel as scheduled tasks (run as the logged-in user, highest privileges).
# Usage (admin PowerShell):  install.ps1 -NgrokAuthToken <token> -NgrokDomain <name.ngrok-free.app>
param([string]$NgrokAuthToken, [string]$NgrokDomain, [int]$Port = 8787)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$raw = 'https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/main/pcmcp'
$dir = Join-Path $env:ProgramData 'pcmcp'
$ua = 'Mozilla/5.0'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an Administrator PowerShell.' }
if (-not $NgrokAuthToken) { $NgrokAuthToken = Read-Host 'ngrok authtoken' }
if (-not $NgrokDomain) { $NgrokDomain = Read-Host 'ngrok static domain (e.g. name.ngrok-free.app)' }
$NgrokDomain = $NgrokDomain -replace '^https?://', '' -replace '/.*$', ''

New-Item -ItemType Directory -Path $dir -Force | Out-Null
foreach ($f in 'server.ps1', 'run-tunnel.ps1', 'uninstall.ps1') {
    Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Uri "$raw/$f`?v=$(Get-Random)" -OutFile (Join-Path $dir $f)
}

# secret token (kept across reinstalls so the connector URL stays valid)
$tokFile = Join-Path $dir 'token.txt'
if (-not (Test-Path $tokFile)) {
    $b = New-Object byte[] 24
    (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($b)
    ([BitConverter]::ToString($b) -replace '-', '').ToLower() | Set-Content $tokFile -Encoding ASCII
}
$token = (Get-Content $tokFile -Raw).Trim()

# ngrok (try native arm64, fall back to x64 which runs under emulation)
$ngrok = Join-Path $dir 'ngrok.exe'
if (-not (Test-Path $ngrok)) {
    $ok = $false
    foreach ($arch in 'arm64', 'amd64') {
        try {
            $zip = Join-Path $env:TEMP "ngrok-$arch.zip"
            Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Uri "https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-windows-$arch.zip" -OutFile $zip
            Expand-Archive -Path $zip -DestinationPath $dir -Force
            if (Test-Path $ngrok) { $ok = $true; Write-Host "ngrok $arch downloaded"; break }
        } catch { Write-Host "ngrok $arch failed: $($_.Exception.Message)" }
    }
    if (-not $ok) { throw 'Could not download ngrok. Download it manually into C:\ProgramData\pcmcp\ngrok.exe' }
}
& $ngrok config add-authtoken $NgrokAuthToken | Out-Null
Set-Content (Join-Path $dir 'domain.txt') $NgrokDomain -Encoding ASCII

# scheduled tasks
$user = "$env:USERDOMAIN\$env:USERNAME"
$princ = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1)
$trig = New-ScheduledTaskTrigger -AtLogOn -User $user
foreach ($t in @(@{ n = 'PCMCP-Server'; f = 'server.ps1'; a = "-Port $Port" }, @{ n = 'PCMCP-Tunnel'; f = 'run-tunnel.ps1'; a = "-Port $Port" })) {
    Unregister-ScheduledTask -TaskName $t.n -Confirm:$false -ErrorAction SilentlyContinue
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$(Join-Path $dir $t.f)`" $($t.a)"
    Register-ScheduledTask -TaskName $t.n -Action $act -Trigger $trig -Principal $princ -Settings $set | Out-Null
    Start-ScheduledTask -TaskName $t.n
}

# local self-test
Start-Sleep -Seconds 4
$local = "http://127.0.0.1:$Port/mcp/$token"
$init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"selftest","version":"1"}}}'
try {
    $r = Invoke-RestMethod -Method Post -Uri $local -Body $init -ContentType 'application/json'
    Write-Host "local server OK: $($r.result.serverInfo.name)"
} catch { Write-Host "local server FAILED: $($_.Exception.Message)  (see $dir\server.log)" }

$url = "https://$NgrokDomain/mcp/$token"
Set-Content (Join-Path $env:USERPROFILE 'Desktop\pcmcp_url.txt') $url -Encoding ASCII
try { Set-Clipboard $url } catch {}
Write-Host ''
Write-Host 'Paste this as MCP server URL in claude.ai > Add custom connector:'
Write-Host $url
Write-Host '(also saved to Desktop\pcmcp_url.txt). Keep it secret: anyone with it controls this PC.'
