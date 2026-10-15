# pcmcp: remote PC control for Claude (MCP over Cloudflare tunnel)

```
Claude (cloud) --HTTPS--> Cloudflare quick tunnel --> cloudflared (Pi) --> server.ps1 (127.0.0.1:8787) --> PowerShell
```

Files in `C:\ProgramData\pcmcp\`: `server.ps1` (MCP server, tools: run_powershell, read_file, write_file, list_dir), `run-tunnel.ps1` (keeps cloudflared up, writes current URL to `Desktop\pcmcp_url.txt`), `cloudflared.exe`, `token.txt` (secret part of the URL = password), `server.log`, `tunnel.log`.
Scheduled tasks `PCMCP-Server` and `PCMCP-Tunnel` start at boot (account GO\go, highest privileges).

Known pitfalls already handled: arm64 cloudflared not available (x64 under emulation), task account must be a mappable name (installer tries SID, COMPUTER\user, user, SYSTEM), raw.githubusercontent cache (use commit hash via `-Ref`), `api.trycloudflare.com` is not the tunnel URL.

## URL changed (Pi or tunnel restarted)
Quick tunnel URLs change on every restart.
```powershell
Get-Content "$env:USERPROFILE\Desktop\pcmcp_url.txt"
```
claude.ai > Settings > Connectors > Pi PC > Remove, then "+" > Name `Pi PC`, paste new URL, Authentication = No sign-in, Add. Start a new chat/session.

Rebuild the URL manually:
```powershell
$t=(Get-Content C:\ProgramData\pcmcp\token.txt -Raw).Trim(); $m=[regex]::Matches([string](Get-Content C:\ProgramData\pcmcp\tunnel.log -Raw),'https://(?!api\.)[a-z0-9-]+\.trycloudflare\.com'); "$($m[$m.Count-1].Value)/mcp/$t"
```

## Connector not working
```powershell
Get-ScheduledTask PCMCP-* | % { "$($_.TaskName) $($_.State)" }; Get-Process cloudflared -EA SilentlyContinue | select Id
Stop-ScheduledTask PCMCP-Tunnel; Get-Process cloudflared -EA SilentlyContinue | Stop-Process -Force; Start-ScheduledTask PCMCP-Server; Start-ScheduledTask PCMCP-Tunnel
Get-Content C:\ProgramData\pcmcp\tunnel.log -Tail 20; Get-Content C:\ProgramData\pcmcp\server.log -Tail 10
```
A 530 from claude.ai right after a tunnel restart usually means the tunnel was not registered yet; wait a minute and retry.

## Reinstall (Administrator PowerShell, also works over SSH)
```powershell
$h=(Invoke-RestMethod "https://api.github.com/repos/autoclawzai/rtl8188eu-woa/commits/main" -UserAgent "x").sha; iwr "https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/$h/pcmcp/install.ps1" -UserAgent "Mozilla/5.0" -OutFile $env:TEMP\pc.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\pc.ps1 -Ref $h
```
The secret token is kept across reinstalls.

## Rotate the secret
```powershell
Remove-Item C:\ProgramData\pcmcp\token.txt
```
then run the reinstall command and re-add the connector with the new URL.

## Stop or remove
```powershell
Stop-ScheduledTask PCMCP-Server; Stop-ScheduledTask PCMCP-Tunnel; Get-Process cloudflared -EA SilentlyContinue | Stop-Process -Force
powershell -ExecutionPolicy Bypass -File C:\ProgramData\pcmcp\uninstall.ps1
```
Also remove the connector in claude.ai.

## Stable URL (needs a domain on Cloudflare)
Zero Trust > Networks > Tunnels > create tunnel, copy the token, add a Public hostname pointing to HTTP `localhost:8787`, then run the installer with `-CfToken "<token>" -CfHostname "mcp.your-domain.com"`.

## Security
The full URL (host + `/mcp/<token>`) is the only credential and gives a elevated PowerShell shell on the PC. Use only on the test machine; rotate if it leaks.
