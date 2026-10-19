# Summarise the last MEMORY.DMP for the restart-hang investigation. Run as admin. Output is short on purpose.
Get-Process ssh -EA 0 | Stop-Process -Force
$d='C:\ProgramData\pcmcp\dbg'; New-Item -ItemType Directory -Force $d | Out-Null
$k='HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_2357&PID_010C\00E04C0001\Device Parameters'
"== boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
$p=Get-ItemProperty $k -EA 0
foreach($n in 'Log_ShutdownNotify','Log_SelfSuspend','Log_SelfSuspend_Shutdown','Log_D0Exit_Count','Log_D0Exit_Target'){ "$n=$($p.$n)" }
$dmp=Get-Item C:\Windows\MEMORY.DMP -EA 0; "dump: $($dmp.LastWriteTime) $([int]($dmp.Length/1MB))MB"
$cmds=@'
.symfix C:\Symbols
.reload
.bugcheck
!analyze -v
!stacks 2 ndis
!stacks 2 WifiCx
!stacks 2 rtl8188eu
!stacks 2 NetAdapterCx
q
'@
Set-Content "$d\scan_cmds.txt" $cmds
$out="$d\scan.txt"
& C:\opencode-dbg\cli-x64\cdb.exe -z C:\Windows\MEMORY.DMP -y "srv*C:\Symbols*https://msdl.microsoft.com/download/symbols" -cf "$d\scan_cmds.txt" > $out 2>&1
"scan.txt lines: " + (Get-Content $out).Count
"== BUGCHECK"; Select-String -Path $out -Pattern '^BUGCHECK_CODE|^BUGCHECK_P[1-4]|^IMAGE_NAME|^FAILURE_BUCKET_ID|^PROCESS_NAME' | select -First 8 | % Line
"== STACK_TEXT"; $t=Get-Content $out; $i=($t | Select-String '^STACK_TEXT' | select -First 1).LineNumber; if($i){ $t[$i..($i+28)] }
"== threads with ndis/WifiCx/rtl8188eu/NetAdapterCx frames"
Select-String -Path $out -Pattern 'ndis!|WifiCx!|rtl8188eu!|NetAdapterCx!|WlanSvc|ndisWait' | select -First 45 | % { $_.Line.Trim() }
