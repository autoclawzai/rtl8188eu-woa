# xlat: static x64 -> ARM64 driver translation (research PoC)

Offline translator that turns an x64 Windows kernel driver (.sys) into ARM64 code.
Goal: reuse vendor x64 drivers on Windows ARM64 without rewriting them.

Input (NOT in repo, vendor binary): `rtwlanu.sys` placed next to the scripts.

- `an.py`   imports / KMDF version / WDI check
- `mix.py`  instruction mix, EH, atomics, privileged-instruction census
- `xlat.py` translator (registers x0-x15 = guest regs, x16-x18 scratch, x4 = rsp), writes poc.s
- `difftest.py` differential test: original x64 vs translated ARM64 in Unicorn (random inputs, compares regs+memory)

Run: `python3 xlat.py && clang -target aarch64-linux-gnu -c poc.s -o poc.o && python3 difftest.py`

Status (phase 1 done): 6569/7920 functions translate; 625/625 differential runs pass on a 679-function closed set.
Roadmap: P2 full instruction coverage, P3 ABI thunks (imports, WDF table, callbacks), P4 ARM64 PE builder + SEH,
P5 hardware load on Pi, P6 hardening/CI, P7 second driver.
