# Strategy: rtl8188eu-woa

Project strategy, Oct 5 to Oct 14, 2026. Ye document batata hai ki TP-Link TL-WN722N v3 (RTL8188EUS) ka Windows 11 ARM64 driver **kaise plan kiya gaya**, har phase ka gate kya tha, kaun se faisle liye gaye aur kyu. Techniques, methodology, chronology aur commands ke alag documents hain.

## 1. Problem aur constraints

**Goal.** Ek native WiFiCx/NetAdapterCx driver jo adapter ko Windows 11 ARM64 par asli Wi-Fi adapter ki tarah dikhaye: scan, open aur WPA2 connect, DHCP, internet, reconnect, usable speed.

- Realtek ne is chip ka ARM64 Windows driver nahi diya. Sirf Linux (`rtl8xxxu`, `rtl8188eu`) aur x86 Windows vendor driver maujood hain.
- User local compile nahi karta. Build sirf GitHub Actions (windows-2022, WDK 10.0.26100) par hota hai.
- Test machine Raspberry Pi 400 hai, kernel debugger nahi. User ki LAN AI sandbox se unreachable hai.
- WiFiCx 1.0 / WDI ke structs ka public documentation kam hai.
- User thakta hai jab tests ki lambi chain aati hai: har round ek reproduction aur ek diagnostic file.

## 2. Phase-gated plan

Har phase ka ek observable pass criterion; agla phase tabhi jab pichla verify ho.

| Phase | Gate (pass criterion) | Approach | Risk |
| --- | --- | --- | --- |
| 1. Bring-up | SYS_CFG padh sakte hain, firmware ready, 507 init writes verify | Working adapter ke capture se init replay; kuch invent nahi | Chip USB par respond karta hai ya nahi |
| 2. Scan | Windows UI mein real BSS list | Pehle passive, phir active scan; WDI BSS entry indications | WiFiCx/WDI API samajhna |
| 3. Open join | DHCP lease open network par | Linux capture se byte-for-byte match | TX descriptor aur datapath format |
| 4. WPA2 | 4-way handshake + internet | Handshake Windows chalata hai; driver sirf CAM mein keys install karta hai | Key install timing, CCMP |
| 5. Speed | Download real Realtek driver ke barabar | Measure, descriptor bisect, A-MPDU, queue sizing | Throughput bottleneck kahan |
| 6. Reliability | Lambe session mein baar-baar reconnect | Pehle instrument, phir fix; evidence se | Intermittent bugs |

Phase 1 ka gate chhota kyunki chip respond na kare to baaki bekaar. SYS_CFG = 0x24403735 padhna pehla proof tha ki USB vendor request 0x05 sahi hai. Phase 4 mein driver ka role chhota: Windows ka supplicant handshake chalata hai; driver EAPOL bina encrypt bhejta hai aur baad mein keys CAM mein dalta hai.

## 3. Chaar sidhant

1. **Jo kaam kar chuka hai use copy karo, invent mat karo.** Firmware aur init tables working adapter ke capture se; TX descriptor aur post-association registers (EDCA, RRSR, INIRTS, TBTT, AGGLEN) Linux aur Realtek driver ke USB traffic se match.
2. **Risk ko alag karo.** Hardware logic `driver.c` (C), Windows framework logic `wifi.cpp` (C++), seema `wifi.h`.
3. **Remote debugging ke liye driver ko khud instrument banao.** Driver registry mein `Log_*` counters likhta hai; user ek script chalata hai jo txt file banati hai.
4. **Sabse anjaan cheez pehle.** WiFiCx 1.0 undocumented tha; CI se WDK headers dump karke padhe (15+ probe commits, Oct 8).

## 4. Faisle aur trade-offs

| Faisla | Alternative | Kyu | Nateeja |
| --- | --- | --- | --- |
| WiFiCx (NetAdapterCx 2.1) | NDIS miniport / WDI | Modern Windows 11 Wi-Fi model | Build chala; 2.1 chahiye tha |
| Softmac: driver khud auth/assoc banata hai | Firmware offload | Firmware sirf bhejta aur sunta hai | Join flow driver mein |
| Windows handshake chalaye | Driver mein supplicant | Kam code, tested path | EAPOL rate bug mila, crypto bug nahi |
| Registry `Cfg_*` knobs | Hard-coded | Rebuild ke bina A/B | Experiments tez |
| Registry `Log_*` counters | Debug prints / ETW | Debugger nahi, script se padhna aasan | Remote diagnosis possible |
| Scripts as files (iwr) | One-liners | Nested quotes toot rahe the | Stable one-command flow |
| LinkState default 0 | LinkState 1 | 1 par lamba no-internet | Default 0 |
| Upload tuning ruk gaya | 41 Mbps tak | Gap ~9 Mbps, risk zyada | 35-37 Mbps stable |

## 5. Priority logic

**kaam karne wala > stable > tez > sundar.** Pehle connect (phases 1-4), phir reliable reconnect, phir speed (measure karke bottleneck todna), cleanup sabse last aur user se poochkar.

## 6. Risks jo pehle pakde

- **Undocumented API:** probe commits.
- **Descriptor over-copy:** poora real descriptor 3 Mbps; `Cfg_DescMask` bisect, best 0x1C.
- **Tooling risk:** Cloudflare ne default curl block kiya (0 MB); byte-count validating harness.
- **CI risk:** failed build ke baad purana release live rehta hai; har push ke baad `gh run list --limit 1`.

## 7. Jo strategy ne nahi kiya

Vendor driver reverse-engineer nahi kiya; power save, hidden SSID, 5 GHz scope se bahar; upload ko 41 Mbps tak push nahi kiya.

## 8. Abhi ka position

Phases 1 se 5 poore. Phase 6 chal raha hai: EAPOL rate fix se reconnect 4 mein se 3 clean. Bacha bug: kuch minute baad connect fail (AP msg1 bhejta hai, Windows msg2 nahi). Agla step: failure live hote hi `diag_now.ps1` capture, guess se fix nahi.
