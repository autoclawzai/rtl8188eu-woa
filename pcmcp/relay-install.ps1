# relay-install.ps1 -Pat <github fine-grained token> -Key <shared secret> [-Ref <sha>]   (run as admin)
param([Parameter(Mandatory)][string]$Pat, [Parameter(Mandatory)][string]$Key, [string]$Ref = 'main')
$ErrorActionPreference = 'Stop'
$dir = 'C:\ProgramData\pcmcp'; New-Item -ItemType Directory -Force $dir | Out-Null
iwr "https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/$Ref/pcmcp/relay.ps1" -UserAgent 'Mozilla/5.0' -OutFile "$dir\relay.ps1"
@{ pat = $Pat; key = $Key } | ConvertTo-Json | Set-Content "$dir\relay.cfg" -Encoding ASCII
icacls "$dir\relay.cfg" /inheritance:r /grant 'SYSTEM:F' 'Administrators:F' | Out-Null
$a = New-ScheduledTaskAction -Execute powershell.exe -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ProgramData\pcmcp\relay.ps1'
$t1 = New-ScheduledTaskTrigger -AtStartup
$t2 = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 5) -RepetitionDuration (New-TimeSpan -Days 3650)
$s = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask PCMCP-Relay -Action $a -Trigger $t1, $t2 -Settings $s -User SYSTEM -RunLevel Highest -Force | Out-Null
Start-ScheduledTask PCMCP-Relay
"relay installed. Remove with: Unregister-ScheduledTask PCMCP-Relay -Confirm:`$false; Stop-Process -Name powershell -EA 0; Remove-Item C:\ProgramData\pcmcp\relay.cfg"
