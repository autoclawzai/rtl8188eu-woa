# Chronology: rtl8188eu-woa

9 din, 131 commits (time git history se, UTC). Commits per day: Oct 5: 6, Oct 7: 9, Oct 8: 37, Oct 9: 17, Oct 10: 23, Oct 11: 9, Oct 12: 16, Oct 13: 14, Oct 14: 3.

| Date | Phase | Commits | Result |
| --- | --- | --- | --- |
| Oct 5 | Start | 6 | CI workflow |
| Oct 7 | Hardware bring-up | 9 | Firmware, 507 init writes, scan |
| Oct 8 | WiFiCx skeleton | 37 | Net adapter, real scan list |
| Oct 9 | Open join | 17 | Association, data path, Linux capture |
| Oct 10 | WPA2 + rate | 23 | WPA2 internet, auto rate, link loss |
| Oct 11 | Throughput | 9 | Async TX, HT40 start |
| Oct 12 | Speed + tooling | 16 | A-MPDU, update.ps1, queue 1024 |
| Oct 13 | Stabilization | 14 | EAPOL rate fix, diagnostics |
| Oct 14 | Documentation | 3 | Docs |

## Oct 5
02:51 initial commit; 03:02-03:18 GitHub Actions workflow (WDK detection, refactor, ARM64 build). CI hi build system.

## Oct 7
01:11 registry logging; 01:48 Phase 3b EFUSE/MAC, power-on, firmware rev 28.0; 02:01 Phase 3c 507 writes replay; 06:01 channel hop, passive scan; 06:18 active scan (bulk OUT 0x02); 15:45 EFUSE TX power, crystal cap; 15:53 Phase 5a structure; 20:32 bulk-IN start/stop fix. Result: SYS_CFG 0x24403735, beacons.

## Oct 8
01:36-01:45 CI probes (WiFiCx headers, MSBuild props, WDI ids); 07:26-07:41 data-path probes, Phase 5b skeleton, NetAdapterCx 2.1; 08:10-08:26 WDI BSS/container struct probes; 12:24 Phase 5c real scan results; 12:26-12:28 new[] removed, `/Zc:sizedDealloc-`; 12:37 RSSI/rate logging; 12:47-17:35 connect/cipher probes.

## Oct 9
03:28 open-network join; 03:54 assoc result needs ActivePhyTypeList + beacon; 04:34 Stopping flag; 04:50 TX worker serialize; 14:29 `Cfg_TxMode`; 15:04 `linux_capture.sh`; 19:14 Linux post-association regs. Result: associate, DHCP reply nahi.

## Oct 10
00:13-01:31 RX histograms, DHCP self-test, frame hex; 01:37 WiFiCx datapath is native 802.11 (breakthrough); 05:21 WPA2 CCMP, CAM; 16:08 disassociation indication; 16:09-17:09 TxRate, HT caps, RSSI auto-rate, pwdb_all fix; 21:51/22:35 rescan after link loss, reverted; 22:49 ADDBA accept (`Cfg_Ba`); 22:59 speed A/B.

## Oct 11
03:48 async TX (8 slots); 04:41 optional USB TX aggregation (off); 14:38-15:15 chip reg 0xFC; 15:45 `capture_x86.ps1`; 23:11 HT40 + real-style descriptor.

## Oct 12
02:20-02:46 `Cfg_DescMask`, 0x1C; 02:54 RX ring 64 to 512; 04:39 `Cfg_QosTx`; 04:57 `update.ps1`; 05:06 TX pipeline diagnostics; 05:29 TX A-MPDU; 12:07-23:05 update.ps1 transcript, SSH-friendly; 23:22-23:39 curl UA, 4 curl processes; 23:52 TX queue 1024. Result: down 36-38.7, up 35.5-36.6 Mbps.

## Oct 13
00:24-00:31 `diag_conn.ps1`; 04:17 `Cfg_LinkState`; 04:36 LinkState 1, auth 8, assoc 6; 04:53 LinkState 0, `diag_now.ps1`; 05:06 `timeline.ps1`; 05:09 EAPOL ring + `eapol.ps1`; 05:14 RX lost-wakeup fix; 11:51 `Cfg_EapolLow` (6M OFDM); 12:16 ring 24 to 128; 17:11 stats timer fix; 17:58 docs.

## Oct 14
Documentation: technical documentation, agent prompt, field report, and strategy/techniques/methodology/chronology/commands documents.

## Reconnect bug, step by step (Oct 13)

| Step | Observation | Decision |
| --- | --- | --- |
| 1 | GUI reconnect par "Can't connect"; ~60 s no internet | `diag_conn.ps1` |
| 2 | LinkState 0 aur 1 dono OK; JoinRuns 5/6 vs 3 | Extra join attempts ka shak |
| 3 | AP deauth ke baad slow | auth 8, assoc 6 |
| 4 | LinkState=1 se lamba no-internet | Default 0 (galat lead) |
| 5 | `diag_now` ne security events filter kiye | `timeline.ps1`: restart har ~6 s |
| 6 | Router har ~6.1 s handshake restart | msg4 reject hypothesis |
| 7 | RX lost-wakeup fix | Rakha, cause nahi (galat lead) |
| 8 | msg4 HT/BW40 descriptor | EAPOL 6M OFDM |
| 9 | Kuch minute baad msg1 aata hai, msg2 nahi | Stats timer fix; live `diag_now` ka intezaar |

## Open items
1. Live-failure `diag_now.ps1`. 2. Na mile to RX watchdog + timestamped stats flush. 3. Cleanup: hidden SSID, diagnostic code, link speed 24 Mbps display, mismatches 0xD70/74/78.
