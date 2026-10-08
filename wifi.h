/* wifi.h - interface between driver.c (hardware, C) and wifi.cpp (WiFiCx / NetAdapterCx, C++) */
#pragma once
#include <ntddk.h>
#include <wdf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call BEFORE WdfDeviceCreate (NetDeviceInitConfig + WifiDeviceInitConfig). */
NTSTATUS WifiCx_DeviceInitConfig(PWDFDEVICE_INIT DeviceInit);

/* Call right AFTER WdfDeviceCreate (allocates wifi context, WifiDeviceInitialize). */
NTSTATUS WifiCx_DeviceInitialize(WDFDEVICE Device);

/* Call from PrepareHardware once the MAC is known: sets WiFiCx device/station/band/PHY capabilities. */
NTSTATUS WifiCx_SetCapabilities(WDFDEVICE Device, const UCHAR Mac[6]);

#ifdef __cplusplus
}
#endif
