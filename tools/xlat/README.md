# xlat: static x64 -> ARM64 driver translation (research PoC)

Offline translator that turns an x64 Windows kernel driver (.sys) into ARM64 code.
Goal: reuse vendor x64 drivers on Windows ARM64 without rewriting them.

Input (NOT in repo, vendor binary): `rtwlanu.sys` placed next to the scripts.

- `an.py`   imports / KMDF version / WDI check
- `mix.py`  instruction mix, EH, atomics, privileged-instruction census
- `xlat.py` translator (registers x0-x15 = guest regs, x16-x18 scratch, x4 = rsp), writes poc.s
- `abi.py`  ABI bridge generator: guest(x64 conv)->native thunk (8 reg args + 8 stack args, saves guest nonvolatiles incl xmm6-15), native->guest entry stubs, indirect-call dispatcher. x18 is never used (Windows reserves it).
- `abi_test.py` Unicorn tests of both directions (9 args, callee-saved preserved)
- `difftest.py` differential test: original x64 vs translated ARM64 in Unicorn (random inputs, compares regs+memory)

Run: `python3 xlat.py && clang -target aarch64-linux-gnu -c poc.s -o poc.o && python3 difftest.py`

Status: P2 done (99.3% functions translate, jump tables, atomics, SSE scalar; fuzz 25k+ runs 0 mismatches). P3 core done (thunks+entry stubs tested). Next: wire data relocations -> entry stubs, imports list, ARM64 PE builder (P4).
Roadmap: P2 full instruction coverage, P3 ABI thunks (imports, WDF table, callbacks), P4 ARM64 PE builder + SEH,
P5 hardware load on Pi, P6 hardening/CI, P7 second driver.
