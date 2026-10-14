# Methodology: rtl8188eu-woa

Kaam karne ka tarika: 9 steps, har phase mein repeat.

**Core loop:** hypothesis -> counter ya script add -> CI build -> user ek command chalaye -> txt file attach -> evidence padho -> fix ya naya hypothesis.

1. **Reference first.** Working adapter ka capture (USBPcap x86 Windows vendor driver, usbmon Linux rtl8xxxu); init sequence, TX descriptor, post-association registers byte-by-byte compare. Example: DHCP reply nahi aaya to Linux ka exact DISCOVER replay kiya.
2. **Smallest verifiable step.** Ek phase = ek observable result (SYS_CFG read, firmware ready, beacon, association, DHCP lease, ping, speed). Unverified phase par agla nahi.
3. **Observable bina debugger ke.** Guess se pehle `Log_*` counter. Ek sawal = ek script = ek Desktop txt.
4. **A/B registry knobs se, rebuild se nahi.** Doubtful behaviour = `Cfg_*` knob; INF default = current best; test script flip/measure/restore.
5. **Trusted measurement harness.** Byte counts validate (0 MB bug), parallel streams, CPU/ring/queue drops note.
6. **Measured bottleneck fix, phir re-measure.**

| Bottleneck | Evidence | Fix | Naya number |
| --- | --- | --- | --- |
| RX ring | 1148 drops | 64 se 512 | drops gaye |
| TX queue | 5320 TxDrop | 256 se 1024 | TxDrop 0 |
| No aggregation | Upload 15-25 vs 41 | A-MPDU | 35-37 Mbps |
| EAPOL descriptor | 6 s handshake loop | 6M OFDM EAPOL | 3 of 4 reconnect clean |

7. **Evidence sahi padho.** Driver ring timestamps ko Windows event logs se correlate karo hypothesis se pehle. Jo nahi dikha wo bhi gino. Galat padhne ke examples: `diag_now.ps1` v1 ne security events filter kiye aur ~30 flapping events chhup gaye; stats counters stuck state ke the hi nahi (timer sirf JOIN_UP mein); RX lost-wakeup fix sahi tha par loop ka cause nahi.
8. **Push to CI -> latest -> update.ps1.**

```
git commit; git push
gh run list --limit 1          # apne commit ka run success hona chahiye
wait 1-2 min                   # release 'latest'
iwr "RAW/<script>.ps1?v=N" -OutFile $env:TEMP\x.ps1; powershell -ExecutionPolicy Bypass -File $env:TEMP\x.ps1
```

Failed build purana release chhod deta hai; raw.githubusercontent ~5 min cache karta hai, isliye `?v=N`.

9. **User ki mehnat kam.** Ek command andar, ek file bahar; ek reproduction aur failure ke dauran ek snapshot.

**Undocumented API:** CI se WDK headers artifact mein dump karo aur signature padho.

## Ek bug ka lifecycle (EAPOL loop)

1. Symptom: reconnect par kabhi ~60 s internet nahi.
2. Repro: `diag_conn.ps1`.
3. Data kam: `timeline.ps1`; security har ~6 s restart.
4. Instrument: EAPOL ring + `eapol.ps1`.
5. Hypothesis: msg4 router ko accept nahi.
6. Galat lead: RX wakeup race.
7. Asli cause: msg4 HT/BW40/SGI/AGG descriptor ke saath.
8. Fix: 6M OFDM plain descriptor.
9. Verify: 4 mein se 3 reconnect turant; JoinRuns 28.
10. Naya failure dikha; loop phir shuru.

## Definition of done

Build green, ek-command install, lambe session (30+ min, kai reconnect, Wi-Fi off/on) mein reconnect repeat, speed regress nahi, docs update.

## Seekh

Counter padhne se pehle dekho wo kab likha gaya. Jo nahi dikha wo bhi evidence hai. Failure ko live pakdo; reboot ya driver restart state mita deta hai.
