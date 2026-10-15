# Source reference (auto-extracted from the code at HEAD)


## driver.c (2721 lines): function index

| Line | Signature | Preceding comment |
| --- | --- | --- |
| 246 | `static NTSTATUS RtlCtrl(PDEVICE_CONTEXT ctx, BOOLEAN read, USHORT reg, PVOID buf, USHORT len)` |  |
| 273 | `static NTSTATUS Rtl_Read8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  *v) { return RtlCtrl(c, TRUE,  r, v, 1); }` |  |
| 274 | `static NTSTATUS Rtl_Read16(PDEVICE_CONTEXT c, USHORT r, USHORT *v) { return RtlCtrl(c, TRUE,  r, v, 2); }` |  |
| 275 | `static NTSTATUS Rtl_Read32(PDEVICE_CONTEXT c, USHORT r, ULONG  *v) { return RtlCtrl(c, TRUE,  r, v, 4); }` |  |
| 276 | `static NTSTATUS Rtl_Write8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  v) { return RtlCtrl(c, FALSE, r, &v, 1); }` |  |
| 277 | `static NTSTATUS Rtl_Write16(PDEVICE_CONTEXT c, USHORT r, USHORT v) { return RtlCtrl(c, FALSE, r, &v, 2); }` |  |
| 278 | `static NTSTATUS Rtl_Write32(PDEVICE_CONTEXT c, USHORT r, ULONG  v) { return RtlCtrl(c, FALSE, r, &v, 4); }` |  |
| 281 | `static NTSTATUS Rmw8(PDEVICE_CONTEXT c, USHORT r, UCHAR clr, UCHAR set)` | read-modify-write: v = (v & ~clr) / set |
| 288 | `static NTSTATUS Rmw16(PDEVICE_CONTEXT c, USHORT r, USHORT clr, USHORT set)` |  |
| 295 | `static NTSTATUS Rmw32(PDEVICE_CONTEXT c, USHORT r, ULONG clr, ULONG set)` |  |
| 305 | `static VOID RegLog(WDFDEVICE dev, PCWSTR name, ULONG value)` |  |
| 316 | `static ULONG RegGetUlong(WDFDEVICE dev, PCWSTR name, ULONG def)` |  |
| 329 | `static VOID RegLogStr(WDFDEVICE dev, PCWSTR name, PCWSTR value)` |  |
| 343 | `static NTSTATUS EfuseRead8(PDEVICE_CONTEXT c, USHORT addr, UCHAR *out)` |  |
| 367 | `static NTSTATUS EfuseParse(PDEVICE_CONTEXT c, ULONG *rawLen)` | Realtek EFUSE format: header byte = (offset<<4 / wordmask) or extended header (low 5 bits 0x0F).  A SET bit in wordmask means the word is NOT present.  Verified |
| 406 | `static NTSTATUS Rtl_ReadEfuse(PDEVICE_CONTEXT c, ULONG *rawLen)` |  |
| 417 | `static NTSTATUS Rtl_PowerOn(PDEVICE_CONTEXT c)` |  |
| 454 | `static NTSTATUS Reset8051(PDEVICE_CONTEXT c)` |  |
| 462 | `static NTSTATUS Rtl_DownloadFirmware(PDEVICE_CONTEXT c, ULONG *reg80, ULONG *polls)` |  |
| 521 | `static NTSTATUS Rtl_ReplayInit(PDEVICE_CONTEXT c, ULONG *done)` |  |
| 541 | `static NTSTATUS Rtl_VerifyInit(PDEVICE_CONTEXT c, WDFDEVICE dev, ULONG *mismatch, ULONG *firstReg, ULONG *firs` | Read back every 32-bit baseband register that the table wrote exactly once and compare.  Informational: some bits may legitimately differ. |
| 565 | `static VOID SleepMs(LONG ms)` |  |
| 572 | `static NTSTATUS Rtl_ReplayInit2(PDEVICE_CONTEXT c, ULONG *done)` |  |
| 593 | `static NTSTATUS Rtl_SetChannel(PDEVICE_CONTEXT c, UCHAR ch)` | RF register 0x18 (channel/bandwidth), path A, via the LSSI write port 0x840: word = (rf_reg << 20) / data.  Capture: ch1 -> 0x01807c01, ch2 -> 0x01807c02 ... |
| 602 | `static LONG Rx_Rssi(const UCHAR *phy, ULONG rate)` | RSSI (dBm) from the RX descriptor PHY status (drvinfo). Formulas follow the Realtek reference driver / rtl8xxxu: CCK uses the AGC report (LNA/VGA index), OFDM/H |
| 627 | `static VOID Scan_AddBss(PDEVICE_CONTEXT c, const UCHAR *bssid, const UCHAR *ssid, UCHAR ssidLen, UCHAR bssCh, ` |  |
| 678 | `static VOID Scan_ProcessFrame(PDEVICE_CONTEXT c, const UCHAR *f, ULONG len, UCHAR rxCh, LONG rssi, const UCHAR` |  |
| 705 | `static VOID Scan_ParseBuffer(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len, UCHAR rxCh)` |  |
| 751 | `static ULONG Rtl_ChannelToGroup(UCHAR ch)` | TX power group, same mapping the capture shows (and rtl8xxxu gen2 uses): ch1-2 ->0, 3-5 ->1, 6-8 ->2, 9-11 ->3, 12-13 ->4, 14 ->5 |
| 762 | `static VOID Rtl_TxPowerForChannel(PDEVICE_CONTEXT c, UCHAR ch, UCHAR *cck, UCHAR *ht)` | cck / ht40(=ofdm,mcs) base index for a channel, from the EFUSE map |
| 775 | `static NTSTATUS Rtl_TxChannelSetup(PDEVICE_CONTEXT c, UCHAR ch)` | Per-channel TX power / band regs, same writes the capture did on every hop. Power values now come from EFUSE. 0xE08 low byte (0x2D) and the 0x0390 upper half we |
| 800 | `static ULONG Rtl_TxPowerSelfCheck(PDEVICE_CONTEXT c)` | Self-check: EFUSE-derived power must equal what the capture wrote for this adapter (cck: ch1-2 0x2f, ch3-8 0x2e, ch9-13 0x2d; ht40: 0x32 / 0x31 / 0x30). |
| 814 | `static NTSTATUS Rtl_ApplyCrystal(PDEVICE_CONTEXT c, ULONG *capOut, ULONG *regOut)` | Crystal cap -> AFE_XTAL_CTRL bits 11..22 = cap / cap<<6 (capture: cap 0x3f gives 0xfff there) |
| 832 | `static NTSTATUS Rtl_SendProbe(PDEVICE_CONTEXT c, UCHAR ch, const UCHAR *ssid, UCHAR ssidLen)` | Probe request. ssid==NULL/len 0 -> wildcard (as captured); otherwise directed. frame = 24 byte header + SSID IE + the captured tail (rates, ext rates, DS, HT ca |
| 887 | `static VOID LogBssList(WDFDEVICE dev, PDEVICE_CONTEXT c)` |  |
| 920 | `VOID EvtUsbRxComplete(WDFUSBPIPE pipe, WDFMEMORY mem, size_t n, WDFCONTEXT ctx)` |  |
| 938 | `BOOLEAN EvtUsbReadersFailed(WDFUSBPIPE pipe, NTSTATUS status, USBD_STATUS usbdStatus)` |  |
| 949 | `static VOID SnapPhase(PDEVICE_CONTEXT c, ULONG i)` |  |
| 961 | `static NTSTATUS Rtl_Bw20Restore(PDEVICE_CONTEXT c)` | ---- HT40 (register values captured from the real Realtek driver: AP ch11, secondary below, centre 9) ---- |
| 974 | `static NTSTATUS Rtl_Bw40Apply(PDEVICE_CONTEXT c, UCHAR primary, BOOLEAN minus)` |  |
| 991 | `static UCHAR Join_ApSecondary(PDEVICE_CONTEXT c)` | Looks at the AP's HT Operation IE (from the scan body). Returns 0 = stay 20 MHz, 1 = secondary above, 3 = secondary below. |
| 1015 | `static NTSTATUS ScanHop(PDEVICE_CONTEXT c, UCHAR ch, BOOLEAN withTxSetup)` |  |
| 1026 | `static BOOLEAN ScanProbe(PDEVICE_CONTEXT c, UCHAR ch, const UCHAR *ssid, UCHAR n)` | One TX with consecutive-failure tracking. TRUE = TX path still alive. |
| 1034 | `static ULONG ScanStep(PDEVICE_CONTEXT c)` | Runs one step. Returns the dwell in ms until the next step, 0 when finished (c->ScanStatus tells whether it finished cleanly). |
| 1109 | `static VOID ScanFinish(WDFDEVICE dev, PDEVICE_CONTEXT c)` |  |
| 1148 | `VOID EvtScanTimer(WDFTIMER timer)` |  |
| 1154 | `VOID EvtScanWork(WDFWORKITEM wi)` |  |
| 1167 | `static VOID ScanStart(PDEVICE_CONTEXT c)` |  |
| 1189 | `NTSTATUS Rtl_WifiScanRequest(WDFDEVICE dev)` |  |
| 1199 | `ULONG Rtl_SnapshotBss(WDFDEVICE dev, BSS_ENTRY *out, ULONG max)` |  |
| 1214 | `static USHORT Le16(const UCHAR *p) { return (USHORT)(p[0] / (p[1] << 8)); }` |  |
| 1217 | `static ULONG Tx_PickRate(PDEVICE_CONTEXT c)` | TX rate: fixed when Cfg_TxRate != 0, otherwise chosen from the smoothed RSSI of the AP's frames. |
| 1231 | `static VOID EvtTxDone(WDFREQUEST req, WDFIOTARGET tgt, PWDF_REQUEST_COMPLETION_PARAMS p, WDFCONTEXT ctx)` |  |
| 1243 | `static VOID TxAsync_Init(WDFDEVICE dev, PDEVICE_CONTEXT c)` | Slots for pipelined data TX; created once the bulk OUT pipes are known. |
| 1268 | `static VOID TxAsync_Drain(PDEVICE_CONTEXT c)` | Wait until all in-flight async writes completed (cancel stragglers). |
| 1288 | `static NTSTATUS Tx_Raw(PDEVICE_CONTEXT c, const UCHAR *frame, ULONG fl, BOOLEAN mgmt, BOOLEAN bmc)` | Generic TX through the descriptor. mgmt -> ep 0x02 / queue MGNT, data -> second endpoint / queue BE. Called at PASSIVE_LEVEL only. frame = full 802.11 frame (se |
| 1429 | `static ULONG Tx_BuildData(PDEVICE_CONTEXT c, const UCHAR *frame, ULONG fl, PUCHAR b)` | USB TX aggregation (the Realtek driver does this: UsbTxAggMode=1, UsbTxAggDescNum=6): several [32 byte descriptor + frame] units, each padded to 8 bytes, go out |
| 1469 | `static NTSTATUS Tx_SendBatch(PDEVICE_CONTEXT c, const UCHAR **fr, const ULONG *fl, ULONG cnt)` | Send frames[0..cnt) as one aggregated bulk transfer (async slot). Called at PASSIVE_LEVEL. |
| 1533 | `static VOID Mgmt_Header(PDEVICE_CONTEXT c, UCHAR *f, UCHAR fc0)` |  |
| 1542 | `static NTSTATUS Join_SendAuth(PDEVICE_CONTEXT c)` |  |
| 1553 | `static ULONG Join_BuildAssocReq(PDEVICE_CONTEXT c, UCHAR *f)` | Builds the association request into c->AssocReq (body only, after the 24-byte header, is what WDI wants). |
| 1578 | `static NTSTATUS Join_Wait(PDEVICE_CONTEXT c, ULONG ms)` |  |
| 1587 | `static NTSTATUS Rtl_H2cMediaStatus(PDEVICE_CONTEXT c, BOOLEAN connect, UCHAR role, UCHAR macid)` | H2C media status report (gen2 firmware, 8188e): cmd 0x01, parm = connect / role << 4, macid. |
| 1612 | `static NTSTATUS Cam_WriteEntry(PDEVICE_CONTEXT c, ULONG entry, BOOLEAN group, UCHAR keyId, const UCHAR *mac, c` |  |
| 1632 | `static NTSTATUS Cam_Invalidate(PDEVICE_CONTEXT c, ULONG entry)` |  |
| 1641 | `static NTSTATUS Sec_Enable(PDEVICE_CONTEXT c)` |  |
| 1651 | `static VOID Sec_Reset(PDEVICE_CONTEXT c)` |  |
| 1663 | `static VOID EvtKeyWork(WDFWORKITEM wi)` |  |
| 1704 | `NTSTATUS Rtl_WifiKey(WDFDEVICE dev, ULONG op, BOOLEAN group, UCHAR keyId, const UCHAR *mac, const UCHAR *key16` |  |
| 1721 | `static NTSTATUS Join_HwUp(PDEVICE_CONTEXT c)` |  |
| 1753 | `static VOID Join_HwDown(PDEVICE_CONTEXT c)` |  |
| 1764 | `static VOID Join_StopScan(PDEVICE_CONTEXT c)` |  |
| 1776 | `static NTSTATUS Tx_TestFrame(PDEVICE_CONTEXT c, ULONG w4, ULONG w5, ULONG w2)` | ---- self test: replay the DHCP DISCOVER captured from Linux with the exact Linux descriptor ---- |
| 1806 | `static VOID Join_SelfTest(WDFDEVICE dev, PDEVICE_CONTEXT c)` |  |
| 1822 | `static NTSTATUS Join_Run(WDFDEVICE dev, PDEVICE_CONTEXT c)` |  |
| 1894 | `static VOID EvtJoinWork(WDFWORKITEM wi)` |  |
| 1925 | `static VOID LogDataStats(WDFDEVICE dev, PDEVICE_CONTEXT c)` |  |
| 2008 | `VOID EvtStatsTimer(WDFTIMER timer)` |  |
| 2015 | `static VOID EvtStatsWork(WDFWORKITEM wi)` |  |
| 2044 | `static VOID EvtBaWork(WDFWORKITEM wi)` | AP asks for a Block-Ack agreement (RX direction): accept it so the AP sends A-MPDUs. The hardware answers the BlockAck itself; we only have to send the ADDBA re |
| 2069 | `static VOID Ba_SendAddbaReq(PDEVICE_CONTEXT c)` | TX-direction Block-Ack: the real Realtek driver sends an ADDBA request for TID 0 once traffic flows; the AP's response (status 0) lets the hardware aggregate ou |
| 2081 | `static VOID EvtLossWork(WDFWORKITEM wi)` |  |
| 2092 | `static VOID EvtTxWork(WDFWORKITEM wi)` |  |
| 2150 | `static VOID Eapol_Note(PDEVICE_CONTEXT c, BOOLEAN rx, const UCHAR *x)` | EAPOL timeline for slow-connect analysis. x = 802.1X header (version byte), >= 17 bytes available |
| 2158 | `static VOID Rx_Frame(PDEVICE_CONTEXT c, const UCHAR *f, ULONG len, ULONG v0)` |  |
| 2270 | `static VOID Rx_Dispatch(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len)` |  |
| 2297 | `NTSTATUS Rtl_WifiConnect(WDFDEVICE dev, const UCHAR *bssid, const UCHAR *ssid, ULONG ssidLen, UCHAR ch, const ` |  |
| 2323 | `ULONG Rtl_CfgLinkState(WDFDEVICE dev)` |  |
| 2328 | `VOID Rtl_WifiDisconnect(WDFDEVICE dev)` |  |
| 2335 | `NTSTATUS Rtl_TxEthernet(WDFDEVICE dev, const UCHAR *eth, ULONG len)` |  |
| 2405 | `NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)` |  |
| 2412 | `NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)` |  |
| 2521 | `NTSTATUS EvtDevicePrepareHardware(WDFDEVICE dev, WDFCMRESLIST res, WDFCMRESLIST resTr)` | PrepareHardware: USB plumbing + EFUSE only. Fast, no radio bring-up. |
| 2621 | `NTSTATUS EvtDeviceD0Entry(WDFDEVICE dev, WDF_POWER_DEVICE_STATE prev)` | D0Entry: full radio bring-up. Runs on first start and after every resume. Debug build: failures are logged and the device stays loaded (HwReady stays 0). |
| 2692 | `NTSTATUS EvtDeviceD0Exit(WDFDEVICE dev, WDF_POWER_DEVICE_STATE target)` |  |

## wifi.cpp (1064 lines): function index

| Line | Signature | Preceding comment |
| --- | --- | --- |
| 34 | `static void* WifiAlloc(size_t size)` |  |
| 39 | `inline void* operator new(size_t, void* p) noexcept { return p; }` |  |
| 40 | `inline void  operator delete(void*, void*) noexcept {}` |  |
| 42 | `void* __cdecl operator new(size_t size) noexcept { return WifiAlloc(size); }` |  |
| 44 | `void* __cdecl operator new(size_t size, ULONG_PTR ctx) noexcept` |  |
| 57 | `void __cdecl operator delete(void* p) noexcept            { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }` |  |
| 58 | `void __cdecl operator delete[](void* p) noexcept          { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }` |  |
| 59 | `void __cdecl operator delete(void* p, ULONG_PTR) noexcept   { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }` |  |
| 60 | `void __cdecl operator delete[](void* p, ULONG_PTR) noexcept { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }` |  |
| 116 | `static VOID WLog(WDFDEVICE dev, PCWSTR name, ULONG value)` | small registry logger, same Device Parameters key as driver.c |
| 130 | `static VOID SendIndication(WDFDEVICE Device, const WDI_MESSAGE_HEADER& Orig, UINT16 MessageId, UINT32 Transact` | ------------------------------------------------------------------ indications ------------------------------------------------------------------ |
| 155 | `static VOID SendM4(WDFDEVICE Device, UINT16 CompleteId, const WDI_MESSAGE_HEADER& Orig, NTSTATUS Status)` |  |
| 162 | `static ULONG BuildRsnIe(const UINT8* body, ULONG bl, ULONG auth, UCHAR* out, ULONG outMax)` | Builds an RSN IE (WPA2-PSK, CCMP pairwise) from the AP's beacon RSN IE. Returns length, 0 if the AP has none. |
| 197 | `static NTSTATUS Ndis2Nt(NDIS_STATUS s) { return (s == 0) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER; }` | ------------------------------------------------------------------ SendCommand: control path ------------------------------------------------------------------ |
| 199 | `static VOID EvtWifiDeviceSendCommand(WDFDEVICE Device, WIFIREQUEST Request)` |  |
| 423 | `static VOID IndicateBss(WDFDEVICE Device, PWIFI_CTX ctx, const BSS_ENTRY& b)` | ------------------------------------------------------------------ scan results -> WDI ------------------------------------------------------------------ |
| 454 | `extern "C" VOID WifiCx_OnScanComplete(WDFDEVICE Device)` |  |
| 476 | `static VOID SetNetLink(WDFDEVICE Device, BOOLEAN up)` | optional: mirror the connection state into the NetAdapter link state (Cfg_LinkState=1) |
| 491 | `extern "C" VOID WifiCx_OnConnectResult(WDFDEVICE Device, NTSTATUS Status, USHORT StatusCode, USHORT Aid, const` |  |
| 577 | `extern "C" VOID WifiCx_OnDisconnectDone(WDFDEVICE Device)` |  |
| 586 | `extern "C" VOID WifiCx_OnLinkLost(WDFDEVICE Device, USHORT Reason)` |  |
| 611 | `static VOID EvtTxAdvance(NETPACKETQUEUE q)` | ------------------------------------------------------------------ data path: TX (Ethernet -> driver.c queue), RX (ring -> NetRing) ---------------------------- |
| 655 | `static VOID EvtTxNotify(NETPACKETQUEUE, BOOLEAN) {}` |  |
| 656 | `static VOID EvtTxCancel(NETPACKETQUEUE q)` |  |
| 671 | `static VOID EvtRxAdvance(NETPACKETQUEUE q)` |  |
| 719 | `static VOID EvtRxNotify(NETPACKETQUEUE q, BOOLEAN enabled)` |  |
| 736 | `static VOID EvtRxCancel(NETPACKETQUEUE q)` |  |
| 753 | `static NTSTATUS EvtCreateTxQueue(NETADAPTER, NETTXQUEUE_INIT* Init)` |  |
| 772 | `static NTSTATUS EvtCreateRxQueue(NETADAPTER, NETRXQUEUE_INIT* Init)` |  |
| 793 | `extern "C" VOID WifiCx_OnRxData(WDFDEVICE Device, const UCHAR* Da, const UCHAR* Sa, const UCHAR* EtherType, co` | called by driver.c at DISPATCH_LEVEL for every received data frame |
| 819 | `extern "C" VOID WifiCx_OnRxFrame(WDFDEVICE Device, const UCHAR* Frame, ULONG Len)` |  |
| 840 | `extern "C" VOID WifiCx_ResetRx(WDFDEVICE Device)` |  |
| 849 | `extern "C" VOID WifiCx_GetDataStats(WDFDEVICE Device, ULONG* Out, ULONG Count)` |  |
| 863 | `static NTSTATUS EvtWifiDeviceCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT* Init)` |  |
| 923 | `static NTSTATUS EvtWifiDeviceCreateWifiDirectDevice(WDFDEVICE, WIFIDIRECT_DEVICE_INIT*)` |  |
| 931 | `extern "C" NTSTATUS WifiCx_DeviceInitConfig(PWDFDEVICE_INIT DeviceInit)` | ------------------------------------------------------------------ exported entry points (called from driver.c) ------------------------------------------------ |
| 938 | `extern "C" NTSTATUS WifiCx_DeviceInitialize(WDFDEVICE Device)` |  |
| 990 | `extern "C" NTSTATUS WifiCx_SetCapabilities(WDFDEVICE Device, const UCHAR Mac[6])` |  |

## wifi.h (53 lines): function index

| Line | Signature | Preceding comment |
| --- | --- | --- |
| 28 | `VOID WifiCx_OnConnectResult(WDFDEVICE Device, NTSTATUS Status, USHORT StatusCode, USHORT Aid, const UCHAR *Bss` | phase 5d: driver.c -> wifi.cpp |
| 33 | `VOID WifiCx_OnRxData(WDFDEVICE Device, const UCHAR *Da, const UCHAR *Sa, const UCHAR *EtherType, const UCHAR *` | DISPATCH_LEVEL: one received data frame, already split into Ethernet pieces (da/sa 6 bytes, etype 2 bytes) |
| 39 | `VOID WifiCx_OnRxFrame(WDFDEVICE Device, const UCHAR *Frame, ULONG Len);   /* native 802.11 data frame (no FCS)` |  |
| 42 | `NTSTATUS Rtl_WifiConnect(WDFDEVICE Device, const UCHAR *Bssid, const UCHAR *Ssid, ULONG SsidLen, UCHAR Channel` | phase 5d: wifi.cpp -> driver.c |

## Cfg_* registry knobs

| Knob | Read in | Default in code | Default in INF |
| --- | --- | --- | --- |
| `Cfg_Ba` | driver.c | 1 (field `c->BaEnable`) | - |
| `Cfg_DescMask` | driver.c | 0x1C (field `c->DescMask`) | - |
| `Cfg_EapolLow` | driver.c | 1 (field `c->EapolLow`) | - |
| `Cfg_Ht40` | driver.c | 1 (field `c->Ht40Cfg`) | - |
| `Cfg_LinkState` | driver.c | ? (field `?`) | - |
| `Cfg_QosTx` | driver.c | 0 (field `c->QosTx`) | - |
| `Cfg_RxNative` | driver.c | 1 (field `c->RxNativeMode`) | - |
| `Cfg_SelfTest` | driver.c | ? (field `?`) | - |
| `Cfg_TxAgg` | driver.c | 0 (field `c->TxAggNum`) | - |
| `Cfg_TxAggPos` | driver.c | 0 (field `c->TxAggPos`) | - |
| `Cfg_TxAmpdu` | driver.c | 1 (field `c->AmpduCfg`) | - |
| `Cfg_TxAsync` | driver.c | ? (field `?`) | - |
| `Cfg_TxMode` | driver.c | ? (field `?`) | - |
| `Cfg_TxRate` | driver.c | ? (field `?`) | - |

## Log_* counters (191 names)

`Log_Act_BadCrc`, `Log_Act_BssCount`, `Log_Act_Frames`, `Log_Act_Mgmt`, `Log_Act_NewBss`, `Log_Act_ProbeResp`, `Log_Act_Status`, `Log_Ba_Req`, `Log_Ba_Resp`, `Log_BulkIn`, `Log_BulkOutCount`, `Log_Chip_RegFC`, `Log_D0Entry_Count`, `Log_D0Entry_Prev`, `Log_D0Exit_Count`, `Log_D0Exit_Target`, `Log_Data_Rx`, `Log_Data_RxDrop`, `Log_Data_RxFrames`, `Log_Data_TxDrop`, `Log_Data_TxFail`, `Log_Data_TxOk`, `Log_Dbg_RxNative`, `Log_Dbg_TxAllSinceUp`, `Log_Dbg_TxFirstMs`, `Log_Dbg_TxLastMs`, `Log_Dbg_TxNative`, `Log_Dir_Resp`, `Log_Dir_Sent`, `Log_Dp_LinkSet`, `Log_Dp_RxAdv`, `Log_Dp_RxArm`, `Log_Dp_RxInd`, `Log_Dp_RxNotify`, `Log_Dp_RxQCancel`, `Log_Dp_RxQCreated`, `Log_Dp_RxRingDrop`, `Log_Dp_TxAdv`, `Log_Dp_TxL2HdrLen`, `Log_Dp_TxL2Type`, `Log_Dp_TxL3Type`, `Log_Dp_TxLastSt`, `Log_Dp_TxPkt`, `Log_Dp_TxQCancel`, `Log_Dp_TxQCreated`, `Log_Dp_TxSubmitFail`, `Log_Dp_TxSubmitOk`, `Log_Eapol_N`, `Log_Efuse_RawLen`, `Log_Efuse_Status`, `Log_Fw_Polls`, `Log_Fw_Reg80`, `Log_Fw_Status`, `Log_Ht40_ApSec`, `Log_Ht40_Status`, `Log_Init2_Done`, `Log_Init2_Status`, `Log_Init_Done`, `Log_Init_Status`, `Log_Join_Aid`, `Log_Join_AssocStatus`, `Log_Join_AssocTries`, `Log_Join_AuthStatus`, `Log_Join_AuthTries`, `Log_Join_HwUp`, `Log_Join_Runs`, `Log_Join_Status`, `Log_Key_Adds`, `Log_Key_Alg`, `Log_Key_Dels`, `Log_Key_Dir`, `Log_Key_Id`, `Log_Key_LastGroup`, `Log_Key_LastId`, `Log_Key_LastSt`, `Log_Key_Len`, `Log_Key_Ptk`, `Log_Key_Queue`, `Log_Key_Type`, `Log_Key_Unsupported`, `Log_MacA`, `Log_MacB`, `Log_NumPipes`, `Log_PowerOn_Status`, `Log_Pwr_Mismatch`, `Log_Rate_NCck`, `Log_Rate_NOfdm`, `Log_Rate_Pick11M`, `Log_Rate_Pick24M`, `Log_Rate_PickMcs4`, `Log_Rate_PickMcs7`, `Log_Rate_RssiCck`, `Log_Rate_RssiOfdm`, `Log_Reader_Status`, `Log_Reg0A_Status`, `Log_Reg0A_Value`, `Log_RxH_T0S00`, `Log_RxU_T0S00`, `Log_Rx_Bytes`, `Log_Rx_Callbacks`, `Log_Rx_DataAny`, `Log_Rx_FirstFc`, `Log_Rx_FirstV0`, `Log_Rx_FltBssid`, `Log_Rx_FltDa`, `Log_Rx_FltLlc`, `Log_Rx_FltNoData`, `Log_Rx_FltNotFromDs`, `Log_Rx_FltShort`, `Log_Rx_ReaderFails`, `Log_Rx_StartStatus`, `Log_Rx_StateAfter`, `Log_Rx_StateBefore`, `Log_Rx_StateEnd`, `Log_Rx_Type2`, `Log_Rx_ZeroLen`, `Log_Scan_BadCrc`, `Log_Scan_BssCount`, `Log_Scan_Frames`, `Log_Scan_Mgmt`, `Log_Scan_Runs`, `Log_Scan_Status`, `Log_Sec_RxProt`, `Log_Sec_RxProtDrop`, `Log_Sec_RxProtV0`, `Log_Sec_TxEnc`, `Log_SelectConfig`, `Log_Stage`, `Log_SysCfg_Status`, `Log_SysCfg_Value`, `Log_Test_A_LinuxDesc_RxData`, `Log_Test_B_OurDesc11M_RxData`, `Log_Test_C_LinuxDesc6M_RxData`, `Log_TxAgg_Batches`, `Log_TxAgg_Frames`, `Log_TxAgg_Num`, `Log_TxAgg_Pos`, `Log_TxAmpdu_State`, `Log_TxAmpdu_Tries`, `Log_TxAsync_Fail`, `Log_TxAsync_Sub`, `Log_TxMode_Active`, `Log_TxPipe`, `Log_TxPipeData`, `Log_TxPipe_QueueMax`, `Log_TxPipe_SemBlocked`, `Log_TxPipe_WorkRuns`, `Log_TxRate_Active`, `Log_Tx_Fail`, `Log_Tx_FirstErr`, `Log_Tx_Ok`, `Log_UsbDeviceCreate`, `Log_Verify_Count`, `Log_Verify_FirstGot`, `Log_Verify_FirstReg`, `Log_Verify_Mismatch`, `Log_Verify_Status`, `Log_Wifi_AdapterCreated`, `Log_Wifi_AdapterInitialize`, `Log_Wifi_AdapterTypeRejected`, `Log_Wifi_AssocResultGen`, `Log_Wifi_CapBand`, `Log_Wifi_CapDevice`, `Log_Wifi_CapPhy`, `Log_Wifi_CapStation`, `Log_Wifi_Caps`, `Log_Wifi_CmdCount`, `Log_Wifi_ConnAuth`, `Log_Wifi_ConnBodyLen`, `Log_Wifi_ConnCh`, `Log_Wifi_ConnDone`, `Log_Wifi_ConnExtLen`, `Log_Wifi_ConnSecure`, `Log_Wifi_ConnSsidLen`, `Log_Wifi_ConnStart`, `Log_Wifi_DeviceInitialize`, `Log_Wifi_DisassocGen`, `Log_Wifi_KeyParseFail`, `Log_Wifi_LastCmd`, `Log_Wifi_LinkLost`, `Log_Wifi_LinkStateGen`, `Log_Wifi_NetAdapterCreate`, `Log_Wifi_NetAdapterStart`, `Log_Wifi_OsWdiVersion`, `Log_Wifi_RsnLen`, `Log_Wifi_ScanIndicated`, `Log_Wifi_ScanReq`, `Log_Wifi_ScanTasks`, `Log_Xtal_Cap`, `Log_Xtal_Reg24`, `Log_Xtal_Status`


## Register / constant defines (driver.c)

```c
#define RTL_VENDOR_REQ      0x05
#define REG_SYS_FUNC        0x0002   /* 16-bit, BIT10 = 8051 enable      */
#define REG_APS_FSMCO       0x0004   /* 32-bit, BIT8  = APFM_ONMAC       */
#define REG_EFUSE_CTRL      0x0030
#define REG_MCU_FW_DL       0x0080   /* 32-bit; +2 = page select         */
#define REG_EFUSE_ACCESS    0x00CF
#define REG_SYS_CFG         0x00F0   /* 32-bit; capture showed 0x24403735 */
#define REG_9346CR          0x000A   /* 16-bit; capture showed 0x0020     */
#define FW_HDR_LEN          32
#define FW_SIG_88E          0x88E1
#define FW_PAGE_SIZE        4096
#define FW_BLOCK            196
#define FW_BASE             0x1000
#define RTL_MAX_BSS         24
#define RX_BUF_SIZE         16384
#define RX_DESC_LEN         24
#define REG_AFE_XTAL_CTRL   0x0024
#define TXQ_N               1024
#define TXQ_BUF             1600
#define JOIN_IDLE           0
#define JOIN_AUTH           1
#define JOIN_ASSOC          2
#define JOIN_UP             3
#define REG_MSR_            0x0102
#define REG_BSSID_          0x0618
#define REG_HMTFR_          0x01CC
#define REG_HMBOX_0_        0x01D0
#define REG_BEACON_CTRL_    0x0550
#define REG_BCN_MAX_ERR_    0x055D
#define REG_BCN_PSR_RPT_    0x06A8
#define REG_SLOT_           0x051B
#define REG_RCR_            0x0608
#define REG_CAMCMD_   0x0670
#define REG_CAMW_     0x0674
#define REG_SECCFG_   0x0680
```

## Join state enum and key structs

```c
```