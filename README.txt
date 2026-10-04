BUILD (dev PC, x64 Windows is fine, cross-compiles to ARM64)
1. Visual Studio 2022 + "MSVC ARM64 build tools" + Windows SDK + WDK (matching versions).
2. New project: "Kernel Mode Driver, Empty (KMDF)", name rtl8188eu.
3. Add driver.c to Source Files, rtl8188eu.inf to Driver Files.
4. Configuration: Debug | ARM64.
5. Project Properties > Driver Settings > Target OS = Windows 10 or later, Target Platform = Desktop.
   Driver Signing > Sign Mode = Test Sign.
6. Build. Output: x64\... -> ARM64\Debug\rtl8188eu\ (rtl8188eu.sys, .inf, .cat) and WDKTestCert.

INSTALL (Nabu, Admin CMD)
  bcdedit /set testsigning on        (reboot once if it was off)
  certutil -addstore Root WDKTestCert.cer
  certutil -addstore TrustedPublisher WDKTestCert.cer
  pnputil /add-driver rtl8188eu.inf /install
  (replug adapter)

CHECK
  Device Manager: device should leave "Error" state.
  DebugView (Sysinternals) > Capture > Capture Kernel, look for "rtl8188eu:" lines.
  Expect: 3 pipes (0x81 IN, 0x02 OUT, 0x03 OUT) and SYS_CFG = 0x24403735.
