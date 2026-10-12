# Download bottleneck check: CPU usage + driver RX counters while downloading. Administrator PowerShell. Output: Desktop\dlcpu.txt
$ProgressPreference = 'SilentlyContinue'
$o = "$env:USERPROFILE\Desktop\dlcpu.txt"
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_2357&PID_010C' } | Select-Object -First 1
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
function Cnt { Get-ItemProperty $k | Select-Object Log_Rx_Callbacks,Log_Rx_Bytes,Log_Rx_ReaderFails,Log_Rx_ZeroLen,Log_Data_RxDeliver,Log_Data_RxDrop,Log_Data_Rx* }
"== dl cpu test $(Get-Date) ==  linkspeed=$((Get-NetAdapter -Name 'Wi-Fi').LinkSpeed)" | Out-File $o
for ($r=1; $r -le 2; $r++) {
  $job = Start-Job { $ProgressPreference='SilentlyContinue'; $sw=[Diagnostics.Stopwatch]::StartNew(); $x=Invoke-WebRequest "http://speed.cloudflare.com/__down?bytes=40000000" -UseBasicParsing -TimeoutSec 60; [math]::Round($x.RawContentLength*8/1e6/$sw.Elapsed.TotalSeconds,1) }
  $s = @(); while ($job.State -eq 'Running') { $s += (Get-Counter '\Processor(_Total)\% Processor Time','\Processor(_Total)\% Interrupt Time','\Processor(_Total)\% DPC Time' -SampleInterval 1 -MaxSamples 1).CounterSamples.CookedValue -join ' ' }
  $mbps = Receive-Job $job -Wait; Remove-Job $job
  "round $r down=$mbps Mbps   cpu/int/dpc samples: $($s -join ' | ')" | Tee-Object -FilePath $o -Append
}
"== driver counters ==" | Out-File $o -Append
Get-ItemProperty $k | Format-List Log_* | Out-File $o -Append
"Done -> $o"
