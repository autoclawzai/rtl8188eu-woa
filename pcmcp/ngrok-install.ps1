# ngrok-install.ps1 -Token <ngrok authtoken> -Domain <your-free-static-domain.ngrok-free.app>   (run as admin)
# Replaces the trycloudflare quick tunnel with a tunnel whose URL never changes: https://<Domain>/mcp/<token>
param([Parameter(Mandatory)][string]$Token, [Parameter(Mandatory)][string]$Domain)
$ErrorActionPreference = 'Stop'
$dir = 'C:\ProgramData\pcmcp'; $exe = "$dir\ngrok.exe"
if (-not (Test-Path $exe)) {
  $zip = "$env:TEMP\ngrok.zip"
  foreach ($u in 'https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-windows-arm64.zip', 'https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-windows-amd64.zip') {
    try { iwr $u -UserAgent 'Mozilla/5.0' -OutFile $zip; break } catch { "download failed: $u" }
  }
  Expand-Archive $zip $dir -Force
}
& $exe config add-authtoken $Token --config "$dir\ngrok.yml" | Out-Null
$arg = "http --url=https://$Domain --config $dir\ngrok.yml 127.0.0.1:8787 --log $dir\ngrok.log"
$a = New-ScheduledTaskAction -Execute $exe -Argument $arg
$t = New-ScheduledTaskTrigger -AtStartup
$s = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask PCMCP-Ngrok -Action $a -Trigger $t -Settings $s -User SYSTEM -RunLevel Highest -Force | Out-Null
Stop-ScheduledTask PCMCP-Tunnel -EA 0; Disable-ScheduledTask PCMCP-Tunnel -EA 0 | Out-Null; Get-Process cloudflared -EA 0 | Stop-Process -Force
Start-ScheduledTask PCMCP-Ngrok; Start-Sleep 8
$tok = (Get-Content "$dir\token.txt").Trim()
"MCP URL (never changes): https://$Domain/mcp/$tok"
"Desktop file updated."; "https://$Domain/mcp/$tok" | Set-Content "$env:USERPROFILE\Desktop\pcmcp_url.txt"
