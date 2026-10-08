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

#ifdef __cplusplus
}
#endif
