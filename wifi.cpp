/*
 * wifi.cpp - Phase 5b: WiFiCx / NetAdapterCx skeleton for the RTL8188EUS driver.
 *
 * Goal of 5b: Windows sees a native Wi-Fi adapter (Get-NetAdapter / netsh wlan).
 *   - capabilities (device/station/band/PHY) are reported
 *   - radio state, dot11 reset, scan (empty result), disconnect are accepted
 *   - connect / everything else -> STATUS_NOT_SUPPORTED
 *   - data path queues exist but drop everything (real TX/RX = phase 5e)
 * Hardware access stays in driver.c (C). This file only touches the framework.
 */
#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <netiodef.h>
#include <wificx.h>
#include "dot11wificxintf.h"
#include "dot11wificxtypes.hpp"
#include "TLVGeneratorParser.hpp"
#include "wifi.h"
#include "netringiterator.h"   /* MIT, from microsoft/NetAdapter-Cx-Driver-Samples */

#define WIFI_POOL_TAG 'shiW'

/* ------------------------------------------------------------------ */
/* operator new/delete: the TLV library allocates through these        */
/* (on ARM64 size_t == ULONG_PTR, so delete(void*,ULONG_PTR) also serves as sized delete) */
/* ------------------------------------------------------------------ */
typedef struct _PLACEMENT_NEW_ALLOCATION_CONTEXT {
    size_t cbMaxSize;
    void*  pbBuffer;
} PLACEMENT_NEW_ALLOCATION_CONTEXT, *PPLACEMENT_NEW_ALLOCATION_CONTEXT;

static void* WifiAlloc(size_t size)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, WIFI_POOL_TAG);
}

inline void* operator new(size_t, void* p) noexcept { return p; }
inline void  operator delete(void*, void*) noexcept {}

void* __cdecl operator new(size_t size) noexcept { return WifiAlloc(size); }

void* __cdecl operator new(size_t size, ULONG_PTR ctx) noexcept
{
    if (ctx != 0) {
        PPLACEMENT_NEW_ALLOCATION_CONTEXT pc = (PPLACEMENT_NEW_ALLOCATION_CONTEXT)ctx;
        if (size <= pc->cbMaxSize) {
            RtlZeroMemory(pc->pbBuffer, size);
            return pc->pbBuffer;
        }
        return nullptr;
    }
    return WifiAlloc(size);
}

void __cdecl operator delete(void* p) noexcept            { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }
void __cdecl operator delete[](void* p) noexcept          { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }
void __cdecl operator delete(void* p, ULONG_PTR) noexcept   { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }
void __cdecl operator delete[](void* p, ULONG_PTR) noexcept { if (p) ExFreePoolWithTag(p, WIFI_POOL_TAG); }

/* ------------------------------------------------------------------ */
/* wifi context (second context on the device object)                  */
/* ------------------------------------------------------------------ */
typedef struct _WIFI_CTX {
    WDFDEVICE   Device;
    TLV_CONTEXT Tlv;
    NETADAPTER  Adapters[5];
    UCHAR       Mac[6];
    BOOLEAN     SoftwareRadioOn;
    LONG        CmdCount;
    LONG        LastMsgId;
    volatile LONG ScanPending;      /* a WDI scan task waits for its M4 */
    WDI_MESSAGE_HEADER ScanHdr;     /* header of that task */
} WIFI_CTX, *PWIFI_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WIFI_CTX, GetWifiCtx)

/* small registry logger, same Device Parameters key as driver.c */
static VOID WLog(WDFDEVICE dev, PCWSTR name, ULONG value)
{
    WDFKEY key;
    UNICODE_STRING nm;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    RtlInitUnicodeString(&nm, name);
    (VOID)WdfRegistryAssignULong(key, &nm, value);
    WdfRegistryClose(key);
}

/* ------------------------------------------------------------------ */
/* indications                                                         */
/* ------------------------------------------------------------------ */
static VOID SendIndication(WDFDEVICE Device, const WDI_MESSAGE_HEADER& Orig, UINT16 MessageId,
                           UINT32 TransactionId, NTSTATUS Status, const UCHAR* Tlv, UINT32 TlvSize)
{
    WDFMEMORY mem = WDF_NO_HANDLE;
    PUCHAR buf = nullptr;
    WDF_OBJECT_ATTRIBUTES attr;
    SIZE_T total = sizeof(WDI_MESSAGE_HEADER) + TlvSize;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&attr, NonPagedPoolNx, 0, total, &mem, (PVOID*)&buf))) return;

    RtlZeroMemory(buf, total);
    PWDI_MESSAGE_HEADER h = (PWDI_MESSAGE_HEADER)buf;
    h->PortId        = Orig.PortId;
    h->Reserved      = Orig.Reserved;
    h->Status        = NT_SUCCESS(Status) ? 0 : (UINT32)0xC0000001; /* NDIS_STATUS_FAILURE */
    h->TransactionId = TransactionId;
    h->IhvSpecificId = Orig.IhvSpecificId;
    if (TlvSize && Tlv) RtlCopyMemory(buf + sizeof(WDI_MESSAGE_HEADER), Tlv, TlvSize);

    WifiDeviceReceiveIndication(Device, MessageId, mem);
    WdfObjectDelete(mem);
}

static VOID SendM4(WDFDEVICE Device, UINT16 CompleteId, const WDI_MESSAGE_HEADER& Orig, NTSTATUS Status)
{
    SendIndication(Device, Orig, CompleteId, Orig.TransactionId, Status, nullptr, 0);
}

/* ------------------------------------------------------------------ */
/* SendCommand: control path                                           */
/* ------------------------------------------------------------------ */
static NTSTATUS Ndis2Nt(NDIS_STATUS s) { return (s == 0) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER; }

static VOID EvtWifiDeviceSendCommand(WDFDEVICE Device, WIFIREQUEST Request)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    UINT inLen = 0, outLen = 0;
    UCHAR* buf = (UCHAR*)WifiRequestGetInOutBuffer(Request, &inLen, &outLen);
    UINT16 id = WifiRequestGetMessageId(Request);
    WDI_MESSAGE_HEADER hdr;

    RtlZeroMemory(&hdr, sizeof(hdr));
    if (buf && inLen >= sizeof(WDI_MESSAGE_HEADER)) RtlCopyMemory(&hdr, buf, sizeof(hdr));

    InterlockedIncrement(&ctx->CmdCount);
    ctx->LastMsgId = id;
    WLog(Device, L"Log_Wifi_CmdCount", (ULONG)ctx->CmdCount);
    WLog(Device, L"Log_Wifi_LastCmd", id);

    switch (id) {
    case WDI_TASK_SET_RADIO_STATE: {
        WDI_SET_RADIO_STATE_PARAMETERS p = {};
        NTSTATUS st = Ndis2Nt(ParseWdiTaskSetRadioState(inLen - sizeof(WDI_MESSAGE_HEADER),
                                buf + sizeof(WDI_MESSAGE_HEADER), &ctx->Tlv, &p));
        if (!NT_SUCCESS(st)) {
            CleanupParsedWdiTaskSetRadioState(&p);
            WifiRequestComplete(Request, st, sizeof(WDI_MESSAGE_HEADER));
            return;
        }
        BOOLEAN newState = p.SoftwareRadioState ? TRUE : FALSE;
        CleanupParsedWdiTaskSetRadioState(&p);
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));      /* M3 */

        if (newState != ctx->SoftwareRadioOn) {
            WDI_INDICATION_RADIO_STATUS_PARAMETERS rs = {};
            UINT8* out = nullptr;
            ULONG cb = 0;
            ctx->SoftwareRadioOn = newState;
            rs.RadioState.HardwareState = TRUE;
            rs.RadioState.SoftwareState = newState;
            if (GenerateWdiIndicationRadioStatus(&rs, 0, &ctx->Tlv, &cb, &out) == 0) {
                SendIndication(Device, hdr, WDI_INDICATION_RADIO_STATUS, 0, STATUS_SUCCESS, out, cb);
                FreeGenerated(out);
            }
        }
        SendM4(Device, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, hdr, STATUS_SUCCESS);   /* M4 */
        return;
    }
    case WDI_TASK_DOT11_RESET:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        SendM4(Device, WDI_INDICATION_DOT11_RESET_COMPLETE, hdr, STATUS_SUCCESS);
        return;

    case WDI_TASK_SCAN: {
        /* 5c: SSID/channel filters are ignored for now, we always scan all 13 channels */
        if (InterlockedCompareExchange(&ctx->ScanPending, 1, 0) != 0) {
            WifiRequestComplete(Request, STATUS_INVALID_DEVICE_STATE, sizeof(WDI_MESSAGE_HEADER));
            return;
        }
        ctx->ScanHdr = hdr;
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));      /* M3 */
        NTSTATUS st = Rtl_WifiScanRequest(Device);
        WLog(Device, L"Log_Wifi_ScanReq", (ULONG)st);
        if (!NT_SUCCESS(st)) {                       /* radio not up: finish right away */
            WifiCx_OnScanComplete(Device);
        }
        return;
    }

    case WDI_TASK_DISCONNECT:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        SendM4(Device, WDI_INDICATION_DISCONNECT_COMPLETE, hdr, STATUS_SUCCESS);
        return;

    default:   /* includes WDI_TASK_CONNECT (phase 5d) */
        WifiRequestComplete(Request, STATUS_NOT_SUPPORTED, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* scan results -> WDI                                                 */
/* ------------------------------------------------------------------ */
static VOID IndicateBss(WDFDEVICE Device, PWIFI_CTX ctx, const BSS_ENTRY& b)
{
    if (b.BodyLen < 12) return;                      /* no usable frame body */

    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS p;
    p.DeviceDescriptor.AllocateElements(1, 0);
    if (p.DeviceDescriptor.ElementCount != 1) return;
    p.Optional.DeviceDescriptor_IsPresent = 1;

    WDI_BSS_ENTRY_CONTAINER& e = p.DeviceDescriptor.pElements[0];
    RtlCopyMemory(e.BSSID.Address, b.Bssid, 6);
    if (b.BodyIsResp) {
        e.Optional.ProbeResponseFrame_IsPresent = 1;
        e.ProbeResponseFrame.SimpleAssign(const_cast<UINT8*>(b.Body), b.BodyLen);
    } else {
        e.Optional.BeaconFrame_IsPresent = 1;
        e.BeaconFrame.SimpleAssign(const_cast<UINT8*>(b.Body), b.BodyLen);
    }
    e.SignalInfo.RSSI = b.Rssi;
    e.SignalInfo.LinkQuality = b.Lq;
    e.ChannelInfo.ChannelNumber = b.Ch ? b.Ch : b.RxCh;
    e.ChannelInfo.BandId = WDI_BAND_ID_2400;

    UINT8* out = nullptr;
    ULONG cb = 0;
    if (GenerateWdiIndicationBssEntryList(&p, 0, &ctx->Tlv, &cb, &out) == 0) {
        SendIndication(Device, ctx->ScanHdr, WDI_INDICATION_BSS_ENTRY_LIST, 0, STATUS_SUCCESS, out, cb);
        FreeGenerated(out);
    }
}

extern "C" VOID WifiCx_OnScanComplete(WDFDEVICE Device)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    if (InterlockedCompareExchange(&ctx->ScanPending, 0, 1) != 1) return;   /* nobody is waiting */

    BSS_ENTRY* snap = (BSS_ENTRY*)WifiAlloc(sizeof(BSS_ENTRY) * 24);
    ULONG n = 0;
    if (snap) n = Rtl_SnapshotBss(Device, snap, 24);
    for (ULONG i = 0; i < n; i++) IndicateBss(Device, ctx, snap[i]);
    if (snap) ExFreePoolWithTag(snap, WIFI_POOL_TAG);

    WLog(Device, L"Log_Wifi_ScanIndicated", n);
    SendM4(Device, WDI_INDICATION_SCAN_COMPLETE, ctx->ScanHdr, STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* data path stubs (5e makes these real)                               */
/* ------------------------------------------------------------------ */
static VOID DropAll(NETPACKETQUEUE q, BOOLEAN tx)
{
    NET_RING_COLLECTION const* rings = tx ? NetTxQueueGetRingCollection(q) : NetRxQueueGetRingCollection(q);

    NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rings);
    while (NetPacketIteratorHasAny(&pi)) {
        NetPacketIteratorGetPacket(&pi)->Ignore = 1;
        NetPacketIteratorAdvance(&pi);
    }
    NetPacketIteratorSet(&pi);

    NET_RING_FRAGMENT_ITERATOR fi = NetRingGetAllFragments(rings);
    NetFragmentIteratorAdvanceToTheEnd(&fi);
    NetFragmentIteratorSet(&fi);
}

static VOID EvtTxAdvance(NETPACKETQUEUE q)  { DropAll(q, TRUE); }
static VOID EvtTxNotify(NETPACKETQUEUE, BOOLEAN) {}
static VOID EvtTxCancel(NETPACKETQUEUE q)   { DropAll(q, TRUE); }

static VOID EvtRxAdvance(NETPACKETQUEUE) {}
static VOID EvtRxNotify(NETPACKETQUEUE, BOOLEAN) {}
static VOID EvtRxCancel(NETPACKETQUEUE q)   { DropAll(q, FALSE); }

static NTSTATUS EvtCreateTxQueue(NETADAPTER, NETTXQUEUE_INIT* Init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE q;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtTxAdvance, EvtTxNotify, EvtTxCancel);
    return NetTxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &q);
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER, NETRXQUEUE_INIT* Init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE q;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtRxAdvance, EvtRxNotify, EvtRxCancel);
    return NetRxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &q);
}

/* ------------------------------------------------------------------ */
/* adapter creation                                                    */
/* ------------------------------------------------------------------ */
#ifndef NDIS_LINK_SPEED_UNKNOWN
#define NDIS_LINK_SPEED_UNKNOWN ((ULONG64)(-1))
#endif

static NTSTATUS EvtWifiDeviceCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT* Init)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    NTSTATUS st;
    NETADAPTER adapter = nullptr;

    if (WifiAdapterInitGetType(Init) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        WLog(Device, L"Log_Wifi_AdapterTypeRejected", (ULONG)WifiAdapterInitGetType(Init));
        return STATUS_NOT_SUPPORTED;
    }

    NET_ADAPTER_DATAPATH_CALLBACKS dp;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&dp, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(Init, &dp);

    st = NetAdapterCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &adapter);
    WLog(Device, L"Log_Wifi_NetAdapterCreate", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    st = WifiAdapterInitialize(adapter);
    WLog(Device, L"Log_Wifi_AdapterInitialize", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    /* link layer: 802.11n 1x1 -> 150 Mbit/s nominal */
    NET_ADAPTER_LINK_LAYER_CAPABILITIES ll;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&ll, 150000000ULL, 150000000ULL);
    NetAdapterSetLinkLayerCapabilities(adapter, &ll);
    NetAdapterSetLinkLayerMtuSize(adapter, 1500);

    NET_ADAPTER_LINK_LAYER_ADDRESS addr;
    RtlZeroMemory(&addr, sizeof(addr));
    addr.Length = 6;
    RtlCopyMemory(addr.Address, ctx->Mac, 6);
    NetAdapterSetPermanentLinkLayerAddress(adapter, &addr);
    NetAdapterSetCurrentLinkLayerAddress(adapter, &addr);

    NET_ADAPTER_TX_CAPABILITIES txCap;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&txCap, 1);
    NET_ADAPTER_RX_CAPABILITIES rxCap;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rxCap, 1514, 1);
    NetAdapterSetDataPathCapabilities(adapter, &txCap, &rxCap);

    st = NetAdapterStart(adapter);
    WLog(Device, L"Log_Wifi_NetAdapterStart", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    UINT32 port = WifiAdapterGetPortId(adapter);
    if (port < ARRAYSIZE(ctx->Adapters)) ctx->Adapters[port] = adapter;

    /* not connected yet */
    NET_ADAPTER_LINK_STATE ls;
    NET_ADAPTER_LINK_STATE_INIT(&ls, NDIS_LINK_SPEED_UNKNOWN, MediaConnectStateDisconnected,
                                MediaDuplexStateUnknown, NetAdapterPauseFunctionTypeUnsupported,
                                NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(adapter, &ls);

    WLog(Device, L"Log_Wifi_AdapterCreated", 1);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtWifiDeviceCreateWifiDirectDevice(WDFDEVICE, WIFIDIRECT_DEVICE_INIT*)
{
    return STATUS_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ */
/* exported entry points (called from driver.c)                        */
/* ------------------------------------------------------------------ */
extern "C" NTSTATUS WifiCx_DeviceInitConfig(PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS st = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(st)) return st;
    return WifiDeviceInitConfig(DeviceInit);
}

extern "C" NTSTATUS WifiCx_DeviceInitialize(WDFDEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES attr;
    PWIFI_CTX ctx = nullptr;
    NTSTATUS st;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, WIFI_CTX);
    st = WdfObjectAllocateContext(Device, &attr, (PVOID*)&ctx);
    if (!NT_SUCCESS(st)) return st;
    ctx->Device = Device;

    WIFI_DEVICE_CONFIG cfg;
    WIFI_DEVICE_CONFIG_INIT(&cfg, WDI_VERSION_LATEST, EvtWifiDeviceSendCommand,
                            EvtWifiDeviceCreateAdapter, EvtWifiDeviceCreateWifiDirectDevice);
    st = WifiDeviceInitialize(Device, &cfg);
    WLog(Device, L"Log_Wifi_DeviceInitialize", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    ctx->Tlv.AllocationContext = 0;
    ctx->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(Device);
    WLog(Device, L"Log_Wifi_OsWdiVersion", (ULONG)ctx->Tlv.PeerVersion);
    return STATUS_SUCCESS;
}

/* capability tables: static so pointers stay valid */
static const DOT11_AUTH_CIPHER_PAIR g_UniAlgos[] = {
    { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
    { DOT11_AUTH_ALGO_RSNA_PSK,   DOT11_CIPHER_ALGO_CCMP },
};
static const DOT11_AUTH_CIPHER_PAIR g_MgmtAlgos[] = {
    { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
};
static const WDI_PHY_TYPE g_Phy24[] = { WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
static const WDI_CHANNEL_MAPPING_ENTRY g_Ch24[] = {
    {1, 2412}, {2, 2417}, {3, 2422}, {4, 2427}, {5, 2432}, {6, 2437}, {7, 2442},
    {8, 2447}, {9, 2452}, {10, 2457}, {11, 2462}, {12, 2467}, {13, 2472},
};
static UINT32 g_Width20 = 20;

#define RATE_RT (WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE)
static const WDI_DATA_RATE_ENTRY g_RatesErp[] = {
    {RATE_RT, 2}, {RATE_RT, 4}, {RATE_RT, 11}, {RATE_RT, 22},                 /* 1, 2, 5.5, 11 */
    {RATE_RT, 12}, {RATE_RT, 18}, {RATE_RT, 24}, {RATE_RT, 36},               /* 6, 9, 12, 18  */
    {RATE_RT, 48}, {RATE_RT, 72}, {RATE_RT, 96}, {RATE_RT, 108},              /* 24, 36, 48, 54 */
};
static const WDI_DATA_RATE_ENTRY g_RatesHt[] = {
    {RATE_RT, 13}, {RATE_RT, 26}, {RATE_RT, 39}, {RATE_RT, 52},               /* MCS0-3 (6.5..26) */
    {RATE_RT, 78}, {RATE_RT, 104}, {RATE_RT, 117}, {RATE_RT, 130},            /* MCS4-7 (..65)  */
};

extern "C" NTSTATUS WifiCx_SetCapabilities(WDFDEVICE Device, const UCHAR Mac[6])
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    NTSTATUS st;
    RtlCopyMemory(ctx->Mac, Mac, 6);

    WIFI_DEVICE_CAPABILITIES dc;
    WIFI_DEVICE_CAPABILITIES_INIT(&dc);
    dc.HardwareRadioState = TRUE;
    dc.SoftwareRadioState = TRUE;
    RtlCopyMemory(dc.FirmwareVersion, "1.0.0", sizeof("1.0.0"));
    dc.NumRxStreams = 1;
    dc.NumTxStreams = 1;
    dc.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
    st = WifiDeviceSetDeviceCapabilities(Device, &dc);
    WLog(Device, L"Log_Wifi_CapDevice", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;
    ctx->SoftwareRadioOn = TRUE;

    WIFI_STATION_CAPABILITIES sc;
    WIFI_STATION_CAPABILITIES_INIT(&sc);
    sc.ScanSSIDListSize = 4;
    sc.DesiredSSIDListSize = 1;
    sc.PrivacyExemptionListSize = 1;
    sc.KeyMappingTableSize = 32;
    sc.DefaultKeyTableSize = 4;
    sc.WEPKeyValueMaxLength = 0x20;
    sc.MaxNumPerSTA = 4;
    sc.NumSupportedUnicastAlgorithms = ARRAYSIZE(g_UniAlgos);
    sc.UnicastAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(g_UniAlgos);
    sc.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(g_UniAlgos);
    sc.MulticastDataAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(g_UniAlgos);
    sc.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(g_MgmtAlgos);
    sc.MulticastMgmtAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(g_MgmtAlgos);
    st = WifiDeviceSetStationCapabilities(Device, &sc);
    WLog(Device, L"Log_Wifi_CapStation", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    WIFI_BAND_INFO bi;
    RtlZeroMemory(&bi, sizeof(bi));
    bi.BandID = WDI_BAND_ID_2400;
    bi.BandState = TRUE;
    bi.NumValidPhyTypes = ARRAYSIZE(g_Phy24);
    bi.ValidPhyTypeList = const_cast<WDI_PHY_TYPE*>(g_Phy24);
    bi.NumValidChannelTypes = ARRAYSIZE(g_Ch24);
    bi.ValidChannelTypes = const_cast<WDI_CHANNEL_MAPPING_ENTRY*>(g_Ch24);
    bi.NumChannelWidths = 1;
    bi.ChannelWidthList = &g_Width20;
    WIFI_BAND_CAPABILITIES bc;
    RtlZeroMemory(&bc, sizeof(bc));
    bc.Size = sizeof(bc);
    bc.NumBands = 1;
    bc.BandInfoList = &bi;
    st = WifiDeviceSetBandCapabilities(Device, &bc);
    WLog(Device, L"Log_Wifi_CapBand", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    static WIFI_PHY_INFO phy[2];
    RtlZeroMemory(phy, sizeof(phy));
    phy[0].PhyType = WDI_PHY_TYPE_ERP;
    phy[0].NumberDataRateEntries = ARRAYSIZE(g_RatesErp);
    RtlCopyMemory(phy[0].DataRateList, g_RatesErp, sizeof(g_RatesErp));
    phy[1].PhyType = WDI_PHY_TYPE_HT;
    phy[1].NumberDataRateEntries = ARRAYSIZE(g_RatesHt);
    RtlCopyMemory(phy[1].DataRateList, g_RatesHt, sizeof(g_RatesHt));
    WIFI_PHY_CAPABILITIES pc;
    RtlZeroMemory(&pc, sizeof(pc));
    pc.Size = sizeof(pc);
    pc.NumPhyTypes = ARRAYSIZE(phy);
    pc.PhyInfoList = phy;
    st = WifiDeviceSetPhyCapabilities(Device, &pc);
    WLog(Device, L"Log_Wifi_CapPhy", (ULONG)st);
    return st;
}
