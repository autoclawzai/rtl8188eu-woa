# Keeps the cloudflared tunnel up. Quick mode writes the current public URL to Desktop\pcmcp_url.txt.
param([int]$Port = 8787, [string]$Dir = "$env:ProgramData\pcmcp")
$cf = Join-Path $Dir 'cloudflared.exe'
$mode = (Get-Content (Join-Path $Dir 'mode.txt') -Raw).Trim()
$log = Join-Path $Dir 'tunnel.log'
$tok = (Get-Content (Join-Path $Dir 'token.txt') -Raw).Trim()
$desk = Join-Path $env:USERPROFILE 'Desktop\pcmcp_url.txt'

if ($mode -eq 'quick') {
    # watcher: publish the URL whenever cloudflared prints a new one
    Start-Job -ScriptBlock {
        param($log, $desk, $tok)
        $last = ''
        while ($true) {
            Start-Sleep -Seconds 5
            if (Test-Path $log) {
                $m = [regex]::Matches((Get-Content $log -Raw -ErrorAction SilentlyContinue), 'https://[a-z0-9-]+\.trycloudflare\.com')
                if ($m.Count -gt 0) {
                    $u = $m[$m.Count - 1].Value
                    if ($u -ne $last) { Set-Content $desk "$u/mcp/$tok" -Encoding ASCII; $last = $u }
                }
            }
        }
    } -ArgumentList $log, $desk, $tok | Out-Null
}

while ($true) {
    if ($mode -eq 'named') {
        $t = (Get-Content (Join-Path $Dir 'cf_token.txt') -Raw).Trim()
        & $cf tunnel --no-autoupdate run --token $t *>> $log
    } else {
        & $cf tunnel --no-autoupdate --url "http://127.0.0.1:$Port" *>> $log
    }
    Start-Sleep -Seconds 5
}
