# Commands and Runs: rtl8188eu-woa

Kab, kaun sa exact command, kyu; scripts; runs. `RAW` = `https://raw.githubusercontent.com/autoclawzai/rtl8188eu-woa/main`. Oct 5-12 ka session transcript compact ho chuka hai, isliye un dino ke rows git commit time/message se hain.

## Tools

| Tool | Kahan | Kaam |
| --- | --- | --- |
| git, gh CLI | AI sandbox | commit, push, `gh run list --limit 1` |
| GitHub Actions windows-2022 | cloud | MSBuild ARM64, stampinf, Inf2Cat, signtool, cert, release `latest` |
| CI probe workflow | cloud | WDK 10.0.26100 header signatures |
| iwr + raw.githubusercontent | user PC | scripts ek command se (`?v=N`) |
| Termux SSH (`ssh Go@192.168.1.17`) | user phone | PC par update.ps1 remote |
| USBPcap / usbmon | reference machines | working driver ka USB traffic |
| pnputil, certutil, bcdedit | user PC | driver install, cert, test-signing |
| netsh wlan, Get-WinEvent, registry | user PC | connect/disconnect, events, `Log_*` |
| curl.exe (4 parallel) | user PC | speed |

## Command log (AI side)

| When | Exact command | Why |
| --- | --- | --- |
| 10-05 02:51-03:18 | `git push` (build-driver.yml, windows-2022) | Local compile nahi, CI build |
| 10-07 01:48 | commit "Phase 3b" EFUSE/MAC, power-on, firmware | Chip zinda karna |
| 10-07 02:01 | commit "Phase 3c" 507 writes replay + read-back | Radio-ready |
| 10-08 01:36-08:26 | 15+ commits "CI probe: ..." (`findstr`/`dir` WDK headers) | Undocumented WiFiCx/WDI structs |
| 10-08 07:36 | commit "Use NetAdapterCx 2.1" | `NET_EUI48_ADDRESS` 2.1+ |
| 10-08 12:24 | commit "Phase 5c: real scan results" | Windows list mein networks |
| 10-09 03:28 | commit "5d-A: open-network join" | Pehla connect |
| 10-09 03:54 | commit "assoc result needs ActivePhyTypeList + beacon" | `NDIS_STATUS_INVALID_DATA` |
| 10-09 15:04 | `linux_capture.sh` (usbmon) | Linux adapter ka traffic |
| 10-10 01:37 | commit "WiFiCx datapath is native 802.11" | DHCP TX/RX nahi |
| 10-10 05:21 | commit "5d-B: WPA2 CCMP, CAM" | Secure network |
| 10-10 22:20 | `test_reconnect.ps1` | Reconnect repro |
| 10-10 22:49-22:59 | commit "accept AP ADDBA (Cfg_Ba)" + `test_speed.ps1` | RX aggregation A/B |
| 10-11 03:48 | commit "pipelined async TX (8 slots)" | Upload ~15 Mbps |
| 10-11 15:45 | `capture_x86.ps1` (USBPcapCMD) | Realtek Windows driver capture |
| 10-11 23:11 | commit "HT40 + real-style TX desc" | 20 se 40 MHz |
| 10-12 02:46 | commit "DescMask default 0x1C" + `test_ht40.ps1` | Descriptor bisect |
| 10-12 02:54 | commit "RX staging ring 64 to 512" | 1148 drops |
| 10-12 04:57 | `update.ps1` | Ek-command install |
| 10-12 05:29 | commit "TX A-MPDU: ADDBA request" | Upload gap |
| 10-12 23:32 | `curl -s -L -A "Mozilla/5.0" https://speed.cloudflare.com/...` | Default UA par 0 MB |
| 10-12 23:52 | `sed -i 's/TXQ_N 256/TXQ_N 1024/'; git push; gh run list --limit 1` | 5320 queue-full drops |
| 10-13 00:24 | `iwr "RAW/diag_conn.ps1?v=N" -OutFile $env:TEMP\x.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\x.ps1` | Failed connect diagnostics |
| 10-13 04:17 | commit "Cfg_LinkState + queue cancel counters" | Link state check |
| 10-13 04:36 | LinkState default 1; auth 4 to 8, assoc 4 to 6 | AP slow after deauth |
| 10-13 04:53 | LinkState default 0 + `diag_now.ps1` | no-internet after reconnect |
| 10-13 05:06 | `timeline.ps1` | Events merge |
| 10-13 05:09 | `eapol.ps1` + EAPOL ring | Handshake kaun kya bhejta hai |
| 10-13 05:14 | commit "RX lost-wakeup fix, flush ring" | Notification race |
| 10-13 11:51 | `Cfg_EapolLow`: `rate = 4; real = FALSE` | 6 s handshake loop |
| 10-13 11:51 | `sed -i 's/Ht40Cfg, DescMask;/Ht40Cfg, DescMask, EapolLow;/' driver.c` | Struct field missing, build fail |
| 10-13 12:16 | EAPOL ring `[24]` to `[128]` | Overflow |
| 10-13 17:11 | `sed -i '2039s/.*/ if (c->HwReady) WdfTimerStart(...)/' driver.c` | Stuck-state counters |
| 10-13 17:58 | `cat > docs/TECHNICAL_DOCUMENTATION.md` | Documentation |

## User-side commands

```powershell
# install + test
iwr "RAW/update.ps1?v=N" -OutFile $env:TEMP\u.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\u.ps1 -Test none
# diagnostics during failure
iwr "RAW/diag_now.ps1?v=N" -OutFile $env:TEMP\x.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\x.ps1
```

`?v=N`: raw.githubusercontent ~5 min cache. `-ExecutionPolicy Bypass`: downloaded script default policy se block. File, one-liner nahi: nested quotes toot rahe the.

## Scripts

| Script | Kya karta hai | Kya nikla |
| --- | --- | --- |
| `update.ps1` | download; profiles export; purana driver delete; cert; install; profiles import; test | Ek-command install |
| `test_5b/5c/5d*` | skeleton, scan, open/WPA2 connect | Adapter load, scan, WPA2 |
| `test_reconnect.ps1` | adapter off/on, router off, 4 min state | Link-loss verify |
| `test_speed.ps1` | Cfg_Ba A/B | ADDBA response default on |
| `test_dlcpu.ps1` | CPU + RX counters in download | 1148 drops, ring 512 |
| `test_ht40.ps1` | 6 descriptor variants | 3 Mbps full desc; 0x1C |
| `test_qos.ps1` | QoS TX variants | Upload 15-25 vs 41 |
| `test_agg.ps1` | USB TX aggregation | Gain nahi, off |
| `test_pipe/ampdu.ps1` | 4 parallel curl, A-MPDU off/on | Final speeds |
| `diag_conn.ps1` | connect/disconnect/reconnect + Log_* | JoinRuns 5/6 vs 3 |
| `diag_now.ps1` | snapshot state, IP, Log_*, events | v1 ne flapping chhupaya |
| `timeline.ps1` | WLAN + DHCP + NLA events merge | Security restart har ~6 s |
| `eapol.ps1` | EAPOL ring + Windows events | msg1-msg4 pairs, loop, msg2 missing |
| `capture_x86.ps1`, `linux_capture.sh` | USBPcap / usbmon | Init, descriptors, post-assoc regs |

## Runs log

| Date | Run | Result |
| --- | --- | --- |
| Oct 7 | Phase 3b-3d | SYS_CFG 0x24403735, fw 28.0, 507 writes |
| Oct 8 | test_5b, 5c | Net adapter, real BSS list |
| Oct 9 | test_5d open | INVALID_DATA (fixed); DHCP reply nahi |
| Oct 10 | test_5d_wpa2, test_reconnect | WPA2 internet; link-loss indication |
| Oct 10-11 | test_speed, test_agg, capture_x86 | ADDBA A/B; aggregation gain nahi |
| Oct 12 | test_dlcpu, test_ht40, test_qos | 1148 drops; 3 Mbps; upload gap |
| Oct 12 | test_ampdu | off: down 20-32, up 19-26; on: down 37-38.9, up 35-37, 5320 TX drops; queue 1024: down 36-38.7, up 35.5-36.6, TxDrop 0, QueueMax 786 |
| Oct 12 | harness fix | UA block fixed; bytes=25000000 code=200 |
| Oct 13 | diag_conn, diag_now, timeline | flapping security events |
| Oct 13 | eapol v9, v10 | 52 events, ~6.1 s loop |
| Oct 13 | after Cfg_EapolLow | 88 events, JoinRuns 28; 3 of 4 reconnects immediate |
| Oct 13 | live failure | JoinRuns 42, KeyAdds 44; msg1 aata hai, msg2 nahi; counters stale |
