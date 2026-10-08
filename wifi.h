/* wifi.h - interface between driver.c (hardware, C) and wifi.cpp (WiFiCx / NetAdapterCx, C++) */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "rtl_bss.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call BEFORE WdfDeviceCreate (NetDeviceInitConfig + WifiDeviceInitConfig). */
NTSTATUS WifiCx_DeviceInitConfig(PWDFDEVICE_INIT DeviceInit);

/* Call right AFTER WdfDeviceCreate (allocates wifi context, WifiDeviceInitialize). */
NTSTATUS WifiCx_DeviceInitialize(WDFDEVICE Device);

/* Call from PrepareHardware once the MAC is known: sets WiFiCx device/station/band/PHY capabilities. */
NTSTATUS WifiCx_SetCapabilities(WDFDEVICE Device, const UCHAR Mac[6]);

/* driver.c -> wifi.cpp: a scan run finished (or was cancelled); completes a pending WDI scan task */
VOID  WifiCx_OnScanComplete(WDFDEVICE Device);

/* wifi.cpp -> driver.c */
NTSTATUS Rtl_WifiScanRequest(WDFDEVICE Device);
ULONG    Rtl_SnapshotBss(WDFDEVICE Device, BSS_ENTRY *Out, ULONG Max);

/* phase 5d: driver.c -> wifi.cpp */
VOID WifiCx_OnConnectResult(WDFDEVICE Device, NTSTATUS Status, USHORT StatusCode, USHORT Aid, const UCHAR *Bssid,
                            const UCHAR *AssocReq, ULONG AssocReqLen, const UCHAR *AssocResp, ULONG AssocRespLen);
VOID WifiCx_OnDisconnectDone(WDFDEVICE Device);
VOID WifiCx_OnLinkLost(WDFDEVICE Device, USHORT Reason);
/* DISPATCH_LEVEL: one received data frame, already split into Ethernet pieces (da/sa 6 bytes, etype 2 bytes) */
VOID WifiCx_OnRxData(WDFDEVICE Device, const UCHAR *Da, const UCHAR *Sa, const UCHAR *EtherType,
                     const UCHAR *Payload, ULONG PayloadLen);

/* phase 5d: stats (wifi.cpp -> driver.c registry log): out[0..11] */
VOID WifiCx_GetDataStats(WDFDEVICE Device, ULONG *Out, ULONG Count);

/* phase 5d: wifi.cpp -> driver.c */
NTSTATUS Rtl_WifiConnect(WDFDEVICE Device, const UCHAR *Bssid, const UCHAR *Ssid, ULONG SsidLen, UCHAR Channel,
                         const UCHAR *ExtIe, ULONG ExtIeLen);
VOID     Rtl_WifiDisconnect(WDFDEVICE Device);
NTSTATUS Rtl_TxEthernet(WDFDEVICE Device, const UCHAR *Eth, ULONG Len);

#ifdef __cplusplus
}
#endif
