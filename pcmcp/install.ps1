# pcmcp installer: MCP server + cloudflared tunnel as scheduled tasks (run as the logged-in user, highest privileges).
# Mode A (stable URL, Cloudflare account + a domain on Cloudflare):
#   install.ps1 -CfToken <tunnel token> -CfHostname mcp.yourdomain.com
#   (Zero Trust > Networks > Tunnels > create tunnel > copy token; add Public hostname -> HTTP -> localhost:8787)
# Mode B (no account, URL changes after every restart):
#   install.ps1
param([string]$CfToken, [string]$CfHostname, [int]$Port = 8787)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$raw = 'https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/main/pcmcp'
$dir = Join-Path $env:ProgramData 'pcmcp'
$ua = 'Mozilla/5.0'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an Administrator PowerShell.' }
$CfHostname = $CfHostname -replace '^https?://', '' -replace '/.*$', ''
$mode = if ($CfToken -and $CfHostname) { 'named' } else { 'quick' }
Write-Host "tunnel mode: $mode"

New-Item -ItemType Directory -Path $dir -Force | Out-Null
foreach ($f in 'server.ps1', 'run-tunnel.ps1', 'uninstall.ps1') {
    Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Uri "$raw/$f`?v=$(Get-Random)" -OutFile (Join-Path $dir $f)
}

# secret token (kept across reinstalls)
$tokFile = Join-Path $dir 'token.txt'
if (-not (Test-Path $tokFile)) {
    $b = New-Object byte[] 24
    (New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($b)
    ([BitConverter]::ToString($b) -replace '-', '').ToLower() | Set-Content $tokFile -Encoding ASCII
}
$token = (Get-Content $tokFile -Raw).Trim()

# cloudflared (native arm64 if published, else x64 under emulation)
$cf = Join-Path $dir 'cloudflared.exe'
if (-not (Test-Path $cf)) {
    $ok = $false
    foreach ($arch in 'arm64', 'amd64') {
        try {
            Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Uri "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-$arch.exe" -OutFile $cf
            if ((Get-Item $cf).Length -gt 1MB) { $ok = $true; Write-Host "cloudflared $arch downloaded"; break }
        } catch { Write-Host "cloudflared $arch failed: $($_.Exception.Message)"; Remove-Item $cf -ErrorAction SilentlyContinue }
    }
    if (-not $ok) { throw 'Could not download cloudflared. Put cloudflared.exe into C:\ProgramData\pcmcp manually.' }
}

# tunnel config
Set-Content (Join-Path $dir 'mode.txt') $mode -Encoding ASCII
if ($mode -eq 'named') {
    $cfTokFile = Join-Path $dir 'cf_token.txt'
    Set-Content $cfTokFile $CfToken -Encoding ASCII
    & icacls.exe $cfTokFile /inheritance:r /grant:r "$($env:USERDOMAIN)\$($env:USERNAME):(R)" 'SYSTEM:(F)' 'Administrators:(F)' | Out-Null
    Set-Content (Join-Path $dir 'hostname.txt') $CfHostname -Encoding ASCII
}
Remove-Item (Join-Path $dir 'tunnel.log') -ErrorAction SilentlyContinue

# scheduled tasks
$user = "$env:USERDOMAIN\$env:USERNAME"
$princ = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1)
$trig = New-ScheduledTaskTrigger -AtLogOn -User $user
foreach ($t in @(@{ n = 'PCMCP-Server'; f = 'server.ps1' }, @{ n = 'PCMCP-Tunnel'; f = 'run-tunnel.ps1' })) {
    Stop-ScheduledTask -TaskName $t.n -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $t.n -Confirm:$false -ErrorAction SilentlyContinue
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$(Join-Path $dir $t.f)`" -Port $Port"
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

# public URL
$base = $null
if ($mode -eq 'named') { $base = "https://$CfHostname" }
else {
    Write-Host 'waiting for trycloudflare URL (up to 60 s)...'
    for ($i = 0; $i -lt 30 -and -not $base; $i++) {
        Start-Sleep -Seconds 2
        $lf = Join-Path $dir 'tunnel.log'
        if (Test-Path $lf) {
            $m = [regex]::Match((Get-Content $lf -Raw -ErrorAction SilentlyContinue), 'https://[a-z0-9-]+\.trycloudflare\.com')
            if ($m.Success) { $base = $m.Value }
        }
    }
}
if (-not $base) { Write-Host "No public URL yet. Check $dir\tunnel.log"; return }
$url = "$base/mcp/$token"
Set-Content (Join-Path $env:USERPROFILE 'Desktop\pcmcp_url.txt') $url -Encoding ASCII
try { Set-Clipboard $url } catch {}
Write-Host ''
Write-Host 'Paste this as MCP server URL in claude.ai > Add custom connector:'
Write-Host $url
Write-Host '(also saved to Desktop\pcmcp_url.txt). Keep it secret: anyone with it controls this PC.'
if ($mode -eq 'quick') { Write-Host 'Quick tunnel: this URL changes after every restart; the new one is written to Desktop\pcmcp_url.txt.' }
