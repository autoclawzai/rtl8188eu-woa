# rtl8188eu-woa — Technical Documentation

Native **Windows 11 ARM64 Wi-Fi driver (WiFiCx / NetAdapterCx, KMDF)** for the **TP-Link TL-WN722N v3
(Realtek RTL8188EUS, USB `VID_2357&PID_010C`)**. Test target: Raspberry Pi 400 running Win11 ARM64 with
test-signing on. Built by GitHub Actions (no local compile), installed with a single PowerShell command.

Repo: `autoclawzai/rtl8188eu-woa` (public). Release `latest` always holds the newest signed build.

---------------------------------------------------------------------------------------------------

## 1. Status (as of this document)

| Area | State |
|---|---|
| Bring-up (power-on, firmware download, MAC/BB/RF init tables) | Works |
| Scan (active + passive, real BSS list in Windows UI) | Works |
| Open + WPA2-PSK (CCMP) connect, DHCP, internet | Works |
| Auto TX rate (RSSI based), HT40, SGI, RX A-MPDU (ADDBA response), TX A-MPDU (ADDBA request + AGG_EN) | Works |
| Link-loss indication, reconnect after router power-cycle | Works |
| Throughput | Download ~37–39 Mbps (real Realtek driver 36.8), upload ~35–37 Mbps (real driver 46.7, Intel 41) |
| Reconnect after GUI/netsh disconnect | Mostly fast (3 of 4 clean after the EAPOL fix) |
| **Open bug** | After some minutes of use, connect sometimes fails ("Can't connect", `11006 key exchange failed`): driver sees AP msg1 but Windows never sends msg2. Root cause NOT yet proven (see §6.1) |
| Not done | Hidden SSID, diagnostic-code cleanup, link speed shows 24 Mbps (cosmetic), 3 verify mismatches at regs 0xD70/74/78, WPA3/WPA1/WEP, 5 GHz (hardware is 2.4 GHz only) |

---------------------------------------------------------------------------------------------------

## 2. Repository map (files and what each does)

### Driver sources
| File | Role |
|---|---|
| `driver.c` (~2700 lines) | Hardware core (KMDF): USB plumbing, EFUSE, power-on, firmware download, init-table replay, channel/BW control, scan, join state machine, TX (descriptor building, async USB slots, CCMP header), RX (USB bulk-IN parser, PHY status/RSSI, 802.11→Ethernet), CAM key install, BA (ADDBA), link-loss detection, registry logging, EAPOL timeline ring |
| `wifi.cpp` / `wifi.h` | WiFiCx layer (C++): WDI/TLV command handlers (scan, connect, disconnect, keys, capabilities), NetAdapter creation, **TX/RX packet queues**, RX staging ring, association-result / link-state indications. `wifi.h` is the C↔C++ boundary (`WifiCx_*` and `Rtl_*` calls) |
| `fwdata.h` | Realtek firmware (rev 28.0) embedded as a C array (source: `fw/rtl8188eufw.bin`) |
| `inittab.h`, `inittab2.h` | MAC / baseband / AGC / RF register init tables (507 writes) captured from a working adapter (USB capture) |
| `rtl_bss.h`, `testframe.h` | small shared structs / test frame |
| `netringiterator.h` | local copy of the NetAdapterCx ring-iterator helpers (not in WDK 26100 headers) |
| `rtl8188eu.inf` | Net-class INF for `USB\VID_2357&PID_010C`, WiFiCx/NetAdapterCx, default registry knobs |
| `rtl8188eu.vcxproj` | MSBuild project (NetAdapterCx 2.1, WiFiCx props, `/Zc:sizedDealloc-`) |
| `.github/workflows/build-driver-v4.yml` | CI: windows-2022 runner, WDK toolset detect, ARM64 build, self-signed test cert, sign `.sys`/`.cat`, zip, publish release `latest` |
| `.github/workflows/probe-wdk.yml` | Old helper that dumped WDK headers (used while WiFiCx 1.0 headers were undocumented) |

### Scripts (run on the Windows test PC, PowerShell, auto-elevate)
| Script | Purpose |
|---|---|
| `update.ps1` | **Main entry point.** Downloads release `latest`, backs up Wi-Fi profiles, reinstalls driver (cert → Root + TrustedPublisher, `pnputil`), restores profiles, runs a test. `-Test none` skips tests; default `ampdu`. Logs a transcript (survives SSH drops) |
| `test_ampdu.ps1`, `test_pipe.ps1` | Speed tests: 4 parallel `curl.exe` processes, download and upload, A-MPDU on/off, byte-count validation; output `Desktop\ampdu.txt` / `pipe.txt` |
| `test_speed.ps1`, `test_dlcpu.ps1`, `test_ht40.ps1`, `test_qos.ps1`, `test_agg.ps1` | Older A/B experiments (BA, CPU/ring drops, HT40/DescMask bisect, QoS TX, USB TX aggregation) |
| `test_reconnect.ps1` | Automatic reconnect test |
| `test_5b/5c/5d*.ps1` | Phase tests (skeleton, scan, open join, TX modes, WPA2) |
| `diag_conn.ps1` | Automated connect → `netsh disconnect` → reconnect repro, dumps `Log_*` before/after (`Desktop\diag_conn.txt`) |
| `diag_now.ps1` | Snapshot of all `Log_*` counters + IP + WLAN/DHCP events (`Desktop\diag_now.txt`). **Run it while the failure is live** |
| `timeline.ps1` | Merged WLAN / DHCP / NLA / profile event timeline (`Desktop\timeline.txt`) |
| `eapol.ps1` | Decodes the driver's EAPOL ring (`Log_Eapol_*`) and merges with Windows events (`Desktop\eapol.txt`) |
| `capture_x86.ps1`, `linux_capture.sh` | USBPcap / usbmon capture on a reference machine with the working vendor/Linux driver |

### Docs
`README.txt` (original manual build/install notes), `NOTES_5d.md` (WDI struct research), `notes/realtek-inf-defaults.md`
(Realtek INF defaults for 2357:010C), `docs/` (this file + agent prompt).

---------------------------------------------------------------------------------------------------

## 3. Architecture

```
 Windows (WLAN AutoConfig / WDI)
        │  WDI TLV commands (scan, connect, disconnect, set keys, ...)        ▲ indications (BSS entries,
        ▼                                                                      │ association result, link state)
 ┌──────────────────────────── wifi.cpp (WiFiCx) ──────────────────────────────────────────┐
 │  Command handlers → Rtl_WifiScan / Rtl_WifiConnect / Rtl_WifiDisconnect / key queue       │
 │  TX queue (NetTxQueue) EvtTxAdvance ──► Rtl_TxEthernet()                                  │
 │  RX staging ring (512 frames) ◄── WifiCx_OnRxFrame / OnRxData  ──► EvtRxAdvance ► NetRxQueue│
 └───────────────▲─────────────────────────────────────────────────────┬───────────────────┘
                 │ WifiCx_On* callbacks                                  │ Rtl_* calls
 ┌───────────────┴──────────────────── driver.c (hardware core) ───────▼───────────────────┐
 │ Work items: JoinWork (scan/join/leave jobs), TxWork (driver TX queue 1024 frames),        │
 │             KeyWork (CAM), BaWork (ADDBA), LossWork (link loss), StatsWork (registry logs)│
 │ USB: control (vendor req 0x05 reg access), bulk-IN continuous reader, bulk-OUT 0x02 (mgmt)│
 │      and 0x03 (data) with 8 async TX slots                                                │
 └──────────────────────────────────────────┬────────────────────────────────────────────────┘
                                            ▼
                         RTL8188EUS (firmware rev 28.0, HT20/HT40, CAM, A-MPDU)
```

### 3.1 Key design decisions
1. **Two layers, one boundary.** Hardware knowledge lives in `driver.c` (plain C). Everything that touches WDI/NetAdapterCx lives in `wifi.cpp`. `wifi.h` is the only contract. This let hardware bring-up (phases 1–4) be debugged with no NetAdapter at all.
2. **MAC-level 802.11 in the driver, not firmware.** The chip is a "softmac" (like Linux `rtl8xxxu`): the driver itself sends auth/assoc/probe frames, parses beacons, builds RSN/HT IEs, and decides rates. Firmware only handles H2C media-status, rate adaptation hooks, etc.
3. **Native 802.11 datapath.** The WiFiCx datapath is native 802.11 (TX passthrough of 802.11 frames from Windows, RX delivered as 802.11), so the driver does not translate Ethernet↔802.11 for data (`Cfg_RxNative=1`).
4. **Async work-item model.** Windows callbacks (DISPATCH_LEVEL, can't block) only enqueue; all slow USB work runs in work items. A `Stopping` flag prevents work items calling into WiFiCx during `D0Exit` (fixed a hung driver removal).
5. **Registry as debug channel.** No kernel debugger on the Pi: the driver writes `Log_*` values to the device's `Device Parameters` registry key and PowerShell scripts read them. `Cfg_*` values are tunable knobs (INF sets defaults) so experiments need no rebuild.
6. **Developer ergonomics first.** User does not compile. CI builds, signs and publishes `latest`; `update.ps1` does the rest. Every diagnostic is a script that writes one text file the user can upload.

### 3.2 Join flow (`Join_Run`, driver.c)
ScanHop to AP channel → apply BW40 if AP is HT40 → Auth (up to 8 tries, 150 ms wait) → Assoc (up to 6 tries, 300 ms) → `Join_HwUp` (BSSID/MSR, H2C media status connected, post-association regs: EDCA, RRSR, INIRTS, TBTT, AGGLEN, RCR) → `JOIN_UP`. Then `WifiCx_OnConnectResult` sends the WDI association result (`PortAuthorized=FALSE` for WPA2) + link state. Windows runs the 4-way handshake **through our datapath**, then sets keys.
Leave: deauth (reason 3) → `Join_HwDown` (Sec_Reset, media status off) → `WifiCx_OnDisconnectDone`. RX data frames are only accepted while `JoinState == JOIN_UP`.

### 3.3 Security (WPA2-CCMP)
- Windows supplies the PTK/GTK via `WDI SET_ADD_CIPHER_KEYS`; `EvtKeyWork` installs them into the hardware **CAM** (`REG_CAM_WRITE 0x674`, `REG_CAM_CMD 0x670`, 6 dwords per entry), `Sec_Enable` writes `SECCFG 0x680 = 0xCF`.
- TX: when `PtkInstalled`, unicast data (not EAPOL) gets an 8-byte CCMP header added by the driver; hardware encrypts and appends MIC (descriptor length excludes MIC). **EAPOL is never encrypted by the driver.**
- RX: hardware decrypts; driver strips CCMP header.
- 4-way handshake key-info decode: msg1 `0x008A`, msg2 `0x010A`, msg3 `0x13CA`, msg4 `0x030A`, group msg1/2 `0x1382/0x0302`.

### 3.4 TX descriptor (32 bytes, `d0..d7`)
Checksum = XOR of the 16-bit words of `d0..d7`. `Cfg_DescMask` selects real-Realtek descriptor pieces:
`1` 16-byte pad, `2` AGG_EN (+density 7), `4` BW40 bit (d4), `8` SGI, `16` fallback table (d6/d7), `32` CTS-self. Default `0x1C` (BW40 + SGI + fallback) was the best in A/B tests.
Rate pick (`Tx_PickRate`): MCS7 if RSSI ≥ −62, MCS4 ≥ −70, 24M OFDM ≥ −78, else 11M CCK (8 until an RSSI sample exists). RSSI is tracked per PHY class (OFDM/HT vs CCK).
**EAPOL frames** (`Cfg_EapolLow=1`): 6M OFDM, no BW40/SGI/AGG/fallback.

### 3.5 TX data path and throughput design
`EvtTxAdvance` → `Rtl_TxEthernet` → driver TxQ (**1024 frames**; 256 caused 5320 drops in uploads → TCP backoff) → `EvtTxWork` → `Tx_Raw` → one of **8 async USB write slots** (`TXA_N=8`, semaphore-guarded). TX A-MPDU: driver sends ADDBA Request (TID 0) after ≥100 data frames; only after the AP answers status 0 does it switch to QoS data + AGG_EN. (A-MPDU: upload 19–26 → 35–37 Mbps.)

### 3.6 RX data path
USB bulk-IN continuous reader → parse RX descriptors/PHY status (RSSI: `pwdb_all` is PHY status byte 4) → `WifiCx_OnRxFrame/OnRxData` copy into a **512-entry staging ring** (64 gave 1148 drops in downloads) → `EvtRxNotify` arms notification / `EvtRxAdvance` drains into NetRxQueue fragments. `EvtRxNotify` has a lost-wakeup guard (kicks immediately if the ring is non-empty); `WifiCx_ResetRx` flushes stale frames on every connect.

### 3.7 HT40
BW40 registers, HT capability IE in assoc request, restored on scan/disconnect (`Ht40Cfg`, `Bw40Active`).

### 3.8 Registry knobs (`HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_2357&PID_010C\...\Device Parameters`)
`Cfg_Ba`=1 (accept AP ADDBA), `Cfg_DescMask`=0x1C, `Cfg_EapolLow`=1, `Cfg_Ht40`=1, `Cfg_LinkState`=1 (mirror link state into NetAdapter; default since Oct 21: with 0 the NDIS datapath stays DIVERTED_BECAUSE_MEDIA_DISCONNECTED after the first disconnect, queues are never recreated and every reconnect fails at M2; the old "long no-internet" with 1 predates the NeedPtk gate), `Cfg_QosTx`=0, `Cfg_RxNative`=1, `Cfg_SelfTest`=0, `Cfg_TxAgg`=0 / `Cfg_TxAggPos`=0 (USB TX aggregation, off), `Cfg_TxAmpdu`=1, `Cfg_TxAsync`=1, `Cfg_TxMode`=0, `Cfg_TxRate`=auto.
Logs: `Log_Join_*`, `Log_Key_*`, `Log_Dp_*` (data path: TxQCreated/RxQCreated, TxAdv, RxAdv, RxArm, RxNotify, RxRingDrop, queue cancel counters), `Log_Data_*`, `Log_Rate_*`, `Log_TxAmpdu_*`, `Log_D0Entry_*`, `Log_Eapol_N/T##/I##` (128-entry EAPOL ring).

---------------------------------------------------------------------------------------------------

## 4. Development timeline (phases)

| Phase | Result |
|---|---|
| 1–2 | Skeleton KMDF USB driver, pipes enumerated, `SYS_CFG = 0x24403735` |
| 3b–3d | EFUSE/MAC read, power-on, **firmware download (rev 28.0)**, replay of 507 MAC/BB/AGC/RF writes with read-back verify, RX config, channel hop |
| 4a–4c | Passive scan (bulk-IN), active scan with probe requests (bulk-OUT 0x02), EFUSE-derived TX power and crystal cap, directed probes |
| 5a | Proper driver structure, explicit start/stop of bulk-IN target |
| 5b | **WiFiCx skeleton** (Net-class INF, NetAdapterCx 2.1, WiFiCx props). Needed many "CI probe" commits because WiFiCx 1.0 headers/struct layouts are barely documented: a workflow dumped WDK headers as artifacts to read signatures |
| 5c | Real scan results via WDI BSS entry indications (frame body + RSSI) |
| 5d-A | Open-network join (auth/assoc), data TX/RX path, association result (needed `ActivePhyTypeList` + beacon → fixed `NDIS_STATUS_INVALID_DATA`) |
| 5d-B | WPA2 CCMP: RSN IE, cipher key commands, CAM install, CCMP TX/RX |
| 5e | RSSI auto-rate, HT caps, ADDBA response, async TX pipeline, QoS/A-MPDU TX, HT40, link-loss indication |
| Stabilization | Reconnect, RX ring/TX queue sizing, EAPOL low rate, diagnostics |

---------------------------------------------------------------------------------------------------

## 5. Bugs found and how they were fixed

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | CI could not find WiFiCx build props/headers | WDK 26100 ships WiFiCx headers but no docs | `probe-wdk.yml` artifact dumps; NetAdapterCx **2.1** needed (`NET_EUI48_ADDRESS`) |
| 2 | Compile errors: duplicate sized delete, `operator new[]` | C++ in kernel | `/Zc:sizedDealloc-`, dropped `new[]` overloads, own `netringiterator.h` |
| 3 | `NDIS_STATUS_INVALID_DATA` on association result | Result lacked PHY type list + beacon | Provide `ActivePhyTypeList` and beacon body |
| 4 | Runaway TX resend | TX worker re-entered | Serialize TX worker |
| 5 | Driver removal hung | Work items called WiFiCx in `D0Exit` | `Stopping` flag, flush all work items, drain async TX |
| 6 | DHCP got no reply | TX frame/queue mapping wrong | Native 802.11 TX passthrough; Linux usbmon capture to match descriptors |
| 7 | Wrong signal strength | OFDM/HT RSSI read from wrong byte | `pwdb_all` is PHY status byte 4; per-class RSSI |
| 8 | Real-Realtek full descriptor gave 3 Mbps | Over-copying descriptor pieces | `Cfg_DescMask` bisect → 0x1C |
| 9 | Download drops (1148 ring drops) | RX ring 64 | 512 |
| 10 | Upload 15–25 Mbps | No QoS/aggregation | TX A-MPDU: ADDBA request, AGG_EN, QoS data after status 0 → 35–37 Mbps |
| 11 | Upload TCP backoff, 5320 TxDrop | TX queue 256 | 1024 (TxDrop 0) |
| 12 | Test harness showed 0 MB | Cloudflare blocked curl default UA; `curl -Z` with several `-o NUL` broke | custom UA, 4 separate `Start-Process curl` runs |
| 13 | Script edits not picked up | raw.githubusercontent caches ~5 min | `?v=N` cache-buster on every URL |
| 14 | One-liner diag broke in PowerShell | quote nesting | all diagnostics are script files fetched with `iwr` |
| 15 | Slow reconnect after disconnect | Auth/assoc retries too few (AP slow right after our deauth) | auth tries 4→8, assoc 4→6 |
| 16 | Long "no internet" after reconnect | `Cfg_LinkState=1` (mirroring link state) | default back to 0 |
| 17 | Possible lost RX wakeup delaying msg1 | arm after drain race | `EvtRxNotify` guard + `WifiCx_ResetRx` on connect |
| 18 | AP restarted the 4-way handshake every ~6 s (3–11 cycles, ~40 s before internet) | msg4 sent as data frame with MCS/BW40/SGI/AGG/fallback right while PTK installs; AP did not accept it | EAPOL sent at 6M OFDM with a plain descriptor (`Cfg_EapolLow`) → 3 of 4 test reconnects clean |
| 19 | Cosmetic "Authentication: Unknown" (event 8001) | WDI auth algorithm reporting | judged cosmetic, not fixed |
| 20 | Connect/reconnect loop: 4-way handshake restarted again and again (event 11006), 15 of 30 stress cycles failed, 12-18 joins per connect | Driver sent ARP/DHCP/IGMP data frames unencrypted right after association, before the PTK was installed. The AP answered with deauth reason 1 about 100 ms after join; the driver reported the link lost, Windows issued a new WDI_TASK_CONNECT (ConnTasks == Join_Runs), and the loop fed itself | `NeedPtk` gate in `Tx_Raw`: on an RSN join only EAPOL leaves the radio until the PTK is installed (`GateDrops` counter). Result: 20 of 20 cycles OK, exactly 1 join per cycle, holds up to 240 s without loss |
| 21 | Every shutdown/restart hung ~5 min, then BSOD 0x9F (DRIVER_POWER_STATE_FAILURE) | Dump: WifiCx `DeletePortTypes` -> `NetAdapterStop` -> `ndisPnPIrpStopDevice` waits for an NDIS user handle (`UM_HANDLE` ref 1) while the miniport is already HALTED/STOPPED; no OID pending, no thread inside rtl8188eu. Handle holder is a Windows service (svchost with Wcmsvc), exact cause unproven | Workaround, not a driver fix: Windows shutdown script `wifi-down.ps1` disables the dongle (PnP) before services stop; boot task `PCMCP-WifiBoot` re-enables it (`Enable-PnpDevice`) and connects Airtel_Thanks. Restart now ~16 s, no dump. Also: Wi-Fi did not auto-connect after reboot while Ethernet was up -> `HKLM\SOFTWARE\Policies\Microsoft\Windows\WcmSvc\GroupPolicy\fMinimizeConnections=0`. Driver-level attempts that did NOT help (dump-verified): quiesce in `EvtDeviceSelfManagedIoSuspend` (runs, trace 0x1001/0x1002), failing that callback on shutdown (0x1005; WifiCx still runs `DeletePortTypes`). Dump facts: both WDF queues empty, no wake policy (`NoWakePowerDown`), at S5 WlanSvc sent radio-off (cmd 8) and disconnect (cmd 7), M4s were sent, then 5 min of nothing. Not a language/driver-code issue. In-memory trace ring (`WIFI_CTX.TrMagic 'RTRC'`) added for dump reading. Open: capture WlanSvc user stacks during the hang (needs a SYSTEM task, `shutdown /f` kills user scripts) |
| 22 | After a power cut the router moved to ch1 and Wi-Fi never connected (Join_Status IO_TIMEOUT, auth 9 tries) | Driver switched to HT40 before auth. AP on ch1 uses secondary ABOVE (HT40+), a case never captured from the real driver | Join at 20 MHz and stay at 20 MHz when the AP secondary is above (commit b528f3d). HT40+ is now SOLVED and the default (commits 6c0ed54, b9060fd): join at 20 MHz, after JOIN_UP switch to 40 MHz with the Linux r8188eu register set (0x818 bits 27:26 = 2 when the primary is the LOWER channel, REG_RRSR+2 = 0x28, RF 0x18 BW40|centre = ch+2). The earlier data failure was the wrong 0x818 value (the minus-case one). Result on ch1 HT40+: ~40 Mbps down, 17-37 Mbps up (TX stays 20 MHz; DATA_BW40/DATA_SC via `Cfg_TxSc` showed no gain over noise, default 15 = off). `Cfg_Ht40Plus=0` forces the old 20 MHz fallback |

---------------------------------------------------------------------------------------------------

## 6. Open problems and next steps

### 6.1 Intermittent "can't connect" (RESOLVED 2026-10-16, see bug 20)
The symptoms below were the pre-PTK data frames triggering AP deauths (reason 1) and Windows reconnect loops, not a stuck RX queue. Kept for history.
Observed (17:48–17:50 test): association status 0 every time, AP sends msg1 (rc=1..4 once per second), driver logs the RX, **no TX msg2 is ever logged** (so Windows never got/answered msg1), then `11006 key exchange failed`. In an earlier window no msg1 was logged at all.
Hypotheses (unproven): (a) Windows NetRxQueue stopped being drained (ring full → `RxRingDrop`; notification never re-armed); (b) data path queues cancelled/recreated (EvtRxCancel sets `RxQueue=nullptr` — no notification can fire if a new queue is not created); (c) device idle / D0 cycle (selective suspend) leaving hardware state stale; (d) CAM/security state not reset after a long session.
The stats timer previously ran only while `JOIN_UP`, so the stuck period was never captured. **Now fixed:** timer runs whenever HW is ready, and `diag_now.ps1` prints `Log_D0*`.
**Next action:** wait for the failure, plug Ethernet, run `diag_now.ps1` (do NOT reboot or restart the driver first), compare `Log_Dp_RxQCreated/RxQCancel`, `Log_Dp_RxRingDrop`, `Log_Dp_RxArm/RxNotify/RxAdv`, `Log_D0Entry_Count`.
If the queue turns out to be cancelled/not recreated or notification is lost: add a watchdog (if RX ring non-empty for >N ms with no EvtRxAdvance → re-kick; on queue create reset flags) and make `RxQueue` handling robust.

### 6.2 Cleanup list (ask the owner first)
Hidden SSID support; remove diagnostic code (EAPOL ring, Dbg logs, `Cfg_LinkState` if unused, self-test); reported link speed (24 Mbps); register verify mismatches 0xD70/74/78; optional upload tuning (A-MPDU density, HT40 TX power regs e10/e14=0x30; remaining gap to real driver ≈ 9 Mbps, judged near practical limit/Pi CPU, **not recommended**); tag current version as stable; unit-style regression scripts into one `selftest.ps1`.

---------------------------------------------------------------------------------------------------

## 7. Step-by-step working method (how this project was actually done)

1. **Reference first.** Capture a working adapter (USBPcap on x86 Windows with the vendor driver, `usbmon` on Linux with `rtl8xxxu`) and compare byte-for-byte: init sequences, TX descriptors, post-association registers.
2. **Smallest thing that can be verified.** One phase = one observable result (SYS_CFG read, firmware ready, beacon seen, probe response, association OK, DHCP lease, ping, speed). Never start phase N+1 on an unverified N.
3. **Everything observable without a debugger.** Add `Log_*` registry counters before guessing; one script per question; script writes a single `.txt` to the Desktop that the user attaches.
4. **A/B with registry knobs, not rebuilds.** Any behavior in doubt becomes a `Cfg_*` knob with the INF default = current best; test scripts flip it, run the measurement, restore it.
5. **Measure with a harness you trust.** Validate byte counts (the "0 MB" bug), use multiple parallel streams, record CPU/ring/queue drops alongside speed.
6. **Fix the measured bottleneck, then re-measure.** (ring 64→512, queue 256→1024, QoS+A-MPDU, EAPOL rate.)
7. **Read the evidence the right way.** Correlate driver ring timestamps with Windows event logs (`timeline.ps1`, `eapol.ps1`) before hypothesizing; count what you do NOT see (a missing TX msg2 mattered more than the RX msg1s).
8. **Push → CI → `latest` → `update.ps1`.** Confirm CI is green (`gh run list --limit 1`), wait ~1–2 min for the release, always use a fresh `?v=N` on script URLs.
9. **Keep the user's effort minimal.** One command in, one file out. Do not chain many tests; ask for one reproduction and a diagnostic snapshot taken *during* the failure.

---------------------------------------------------------------------------------------------------

## 8. Gotchas and lessons (extra)

- WiFiCx 1.0 is thinly documented: when a struct/function is unclear, dump the WDK headers via CI (artifact) rather than guessing. WDI structs are in `km/wlan/2.0/TlvGenerated_.hpp`.
- Registry `Log_*` values are only as fresh as the last flush; check *when* a counter is written before trusting it (the stuck-state mistake).
- Windows `netsh wlan` events: 8000/8001/8002/8003 (connect/disconnect), 11000/11001 (assoc), 11010/11005/11004 (security start/ok/stop), **11006 (key exchange failed)**; NetworkProfile 10000/10001/4004; DHCP 50065/50067.
- `raw.githubusercontent.com` caches ~5 min; PowerShell one-liners break on nested quotes; Cloudflare blocks default curl UA; `Invoke-WebRequest` progress bar throttles downloads (`$ProgressPreference='SilentlyContinue'`).
- Sandbox (where the AI works) cannot reach the user's LAN or speed servers: all measurements come from the user's machine.
- Test-signing: `bcdedit /set testsigning on`, certificate into **Root and TrustedPublisher**; `update.ps1` does it.
- Keep `Cfg_LinkState=1` (do not set 0); do not enable `Cfg_TxAgg` (USB TX aggregation) without a reason (no gain measured).
- The adapter is 2.4 GHz only, 1T1R: ~38 Mbps TCP is close to what the real driver gets on this hardware.
- Original README mentions a Nabu (Snapdragon) target; current test machine is a **Raspberry Pi 400**.
