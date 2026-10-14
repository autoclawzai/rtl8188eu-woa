# Techniques: rtl8188eu-woa

18 concrete techniques: har ek ka problem, tareeka, evidence aur result. Strategy, methodology, chronology aur commands alag documents mein hain.

## A. Capture aur reference

1. **USB capture diffing (USBPcap + usbmon).** Problem: init sequence, TX descriptor aur association ke baad ke registers documented nahi. Tareeka: x86 Windows par vendor driver ke saath USBPcap (`capture_x86.ps1`) aur Linux par `rtl8xxxu` ke saath usbmon (`linux_capture.sh`), phir hamare traffic se byte-by-byte compare. Mila: init writes, descriptor layout, EDCA/RRSR/INIRTS/TBTT/AGGLEN (Oct 9). Dhyan: ek hi network condition ka capture lo.
2. **Init-table replay + read-back verify.** 507 MAC/BB/AGC/RF writes; har write ke baad read-back. 3 mismatches (0xD70, 0xD74, 0xD78) verify list mein; chip normal chalta hai.
3. **Firmware rev 28.0 embed.** `fwdata.h` mein; power-on par download (Oct 7, Phase 3b).
4. **EFUSE se MAC, TX power, crystal cap.** Hard-coded nahi (Oct 7, Phase 4c).
5. **CI header probing.** Workflow step WDK 10.0.26100 headers par `findstr`/`dir` chalata hai, result CI log/artifact mein. 15+ probe commits, Oct 8. Mila: BSS entry structs, association result structs, cipher key containers, PHY type list.

## B. Observability

6. **Registry `Log_*` channel.** `Log_Join_Runs`, `Log_Key_Adds`, `Log_Dp_*`, `Log_D0Entry_*`... `Device Parameters` ke neeche. Seema: values utni fresh jitni last flush; isliye stats timer HW-ready par hamesha chalta hai (Oct 13 17:11).
7. **`Cfg_*` knobs se A/B.** `Cfg_Ba`, `Cfg_DescMask`, `Cfg_EapolLow`, `Cfg_Ht40`, `Cfg_LinkState`, `Cfg_QosTx`, `Cfg_TxAgg`, `Cfg_TxAmpdu`, `Cfg_TxMode`, `Cfg_TxRate`. Test script flip, measure, restore.
8. **Self-test: Linux DHCP DISCOVER replay (Oct 10).** Descriptor sahi nikla; bug Windows datapath format mein tha (WiFiCx datapath native 802.11).
9. **Frame hex logging, TX class, RX histograms.** Kaun sa frame jaa raha hai/aa raha hai.
10. **EAPOL ring + `eapol.ps1` / `timeline.ps1`.** 128-entry ring mein har EAPOL TX/RX ka time aur key-info (msg1 0x008A, msg2 0x010A, msg3 0x13CA, msg4 0x030A); Windows events ke saath merge. Mila: router har ~6.1 s handshake restart karta hai.

## C. Performance

11. **`Cfg_DescMask` bisect.** Poora real descriptor sirf 3 Mbps. Pieces: 16B pad, AGG_EN/density, BW40, SGI, fallback, CTS-self. `test_ht40.ps1` se best 0x1C (BW40 + SGI + fallback).
12. **Per-class RSSI rate pick.** MCS7 / MCS4 / 24M / 11M. Bug: OFDM/HT RSSI galat byte se; sahi `pwdb_all` PHY status byte 4.
13. **Async pipelined TX.** 8 async USB slots + semaphore, TX queue 256 se 1024; 1024 par TxDrop 0, QueueMax 786.
14. **A-MPDU gating.** ADDBA request (TID 0), `AGG_EN`, QoS data, contiguous data; tabhi jab AP ne status 0 diya. Upload 19-26 se 35-37 Mbps.

## D. Tooling

15. **4 alag curl processes, byte-count validation.** `curl -Z` aur `-o NUL` toote; 4 alag `Start-Process curl.exe`, custom User-Agent, har ek ka output aur error log.
16. **`update.ps1` + diagnostic scripts.** Profile backup/restore, purana driver delete, cert, install, test, transcript.

## E. Safety

17. **`Stopping` flag, work-item flush, async TX drain.** D0Exit mein work items WiFiCx call karte the aur driver removal hang hota tha.
18. **`EvtRxNotify` lost-wakeup guard + `WifiCx_ResetRx`.** Notification arm hone se pehle frame aaye to wakeup kho sakta tha; connect par stale RX ring flush.

## Asar

| Technique | Sabse bada result |
| --- | --- |
| USB capture diffing | Descriptor aur post-association registers sahi |
| Descriptor bisect | 3 Mbps se best config |
| A-MPDU gating | Upload 19-26 se 35-37 Mbps |
| RX ring 64 se 512 | 1148 drops se kam |
| EAPOL ring | Handshake loop ka asli karan |
| EAPOL 6M OFDM | 3 of 4 reconnects clean |
| Header probing | WiFiCx build possible |
