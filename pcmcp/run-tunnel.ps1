# Keeps the ngrok tunnel up. Tries --url, falls back to --domain for older ngrok versions.
param([int]$Port = 8787, [string]$Dir = "$env:ProgramData\pcmcp")
$ngrok = Join-Path $Dir 'ngrok.exe'
$domain = (Get-Content (Join-Path $Dir 'domain.txt') -Raw).Trim()
$log = Join-Path $Dir 'tunnel.log'
$flag = '--url'
while ($true) {
    $started = Get-Date
    & $ngrok http $Port "$flag=$domain" --log=stdout --log-level=info *>> $log
    if (((Get-Date) - $started).TotalSeconds -lt 15) { $flag = if ($flag -eq '--url') { '--domain' } else { '--url' } }
    Start-Sleep -Seconds 5
}
