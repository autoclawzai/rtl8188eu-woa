# Removes the remote-control server and tunnel completely. Run in Administrator PowerShell.
foreach ($t in 'PCMCP-Server', 'PCMCP-Tunnel') {
    Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue
}
Get-Process ngrok -ErrorAction SilentlyContinue | Stop-Process -Force
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'pcmcp' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item "$env:ProgramData\pcmcp" -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'pcmcp removed.'
