# Decode the driver's persistent timeline: HKLM\...\Enum\USB\VID_2357&PID_010C\<instance>\Device Parameters\Log_Timeline
# Each entry is "ms:code:arg" (hex, ms = ms since boot, oldest first). Heartbeat 0x2060 every 2 s while suspending/shutting down:
# if Log_Hb_Ms keeps growing during a shutdown hang the driver is alive and the hang is above it (NDIS/WlanSvc).
param([string]$Key='HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_2357&PID_010C\00E04C0001\Device Parameters')
$n=@{
 0x1001='SelfManagedIoSuspend';0x1002='  quiesced';0x1003='D0Exit';0x1004='  suspend powerAction';0x1005='  fail-on-shutdown';0x1010='ShutdownNotify';0x1011='  after SetFailed';
 0x1020='quiesced disconnect';0x1100='PrepareHardware';0x1101='D0Entry(prev)';0x1102='  init status';
 0x2001='TXQ created(n)';0x2002='RXQ created(n)';0x2003='TXQ cancel(txpkts)';0x2004='RXQ cancel(rxind)';
 0x2010='LINK UP(txpkts)';0x2011='LINK DOWN(txpkts)';0x2020='first TX pkt after up';0x2021='first RX pkt after up';
 0x2030='EAPOL rx(keyinfo)';0x2031='EAPOL tx(keyinfo)';0x2040='join start(ch)';0x2041='join result(status)';0x2042='leave';0x2043='link lost(reason)';
 0x2050='key(op<<8|grp|id<<16)';0x2060='heartbeat(stopping)'}
$wdi=@{4='SCAN';6='CONNECT';7='DOT11_RESET';8='DISCONNECT';0x1C='cmd1C';0x22='cmd22'}
$t=(Get-ItemProperty $Key).Log_Timeline
if(-not $t){ 'no Log_Timeline'; return }
$prev=$null
foreach($e in ($t.Trim() -split '\s+')){
  $p=$e -split ':'; $ms=[Convert]::ToUInt32($p[0],16); $c=[Convert]::ToUInt32($p[1],16); $a=[Convert]::ToUInt32($p[2],16)
  $name= if($n.ContainsKey([int]$c)){$n[[int]$c]} elseif($c -ge 0x8000){'ind 0x{0:X}' -f ($c-0x8000)} elseif($wdi.ContainsKey([int]$c)){'WDI '+$wdi[[int]$c]} else {'cmd 0x{0:X}' -f $c}
  $d= if($prev){ '+{0,6}ms' -f ($ms-$prev) } else { '        ' }
  '{0,10} {1} {2} arg=0x{3:X}' -f $ms,$d,$name,$a
  $prev=$ms
}
"Log_Hb_Ms = $((Get-ItemProperty $Key).Log_Hb_Ms)"
