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
#include <net/virtualaddress.h>
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
    /* phase 5d */
    volatile LONG ConnPending;      /* a WDI connect task waits for its M4 */
    WDI_MESSAGE_HEADER ConnHdr;
    volatile LONG DiscPending;
    WDI_MESSAGE_HEADER DiscHdr;
    UCHAR       Bssid[6];
    BOOLEAN     Connected;
    UCHAR       ConnChannel;
    UCHAR       ConnBeacon[640];
    ULONG       ConnBeaconLen;
    /* RX ring: Ethernet frames waiting for the NetAdapter rx queue (producer = USB completion, DISPATCH) */
    KSPIN_LOCK  RxLock;
    NETPACKETQUEUE RxQueue;
    volatile LONG RxNotifyArmed;
    ULONG       RxHead, RxTail;
    USHORT      RxLen[64];
    UCHAR       Rx[64][1536];
    ULONG       RxDropped;
} WIFI_CTX, *PWIFI_CTX;

typedef struct _QUEUE_CTX {
    PWIFI_CTX    Wifi;
    NET_EXTENSION VaExt;
} QUEUE_CTX, *PQUEUE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CTX, GetQueueCtx)

static WDFDEVICE g_WifiDevice;     /* one adapter per driver instance */
/* data-path debug counters (logged from driver.c every 2 s while connected) */
enum { ST_TXQ_CREATED, ST_RXQ_CREATED, ST_TX_ADV, ST_TX_PKT, ST_TX_SUBMIT_OK, ST_TX_SUBMIT_FAIL, ST_TX_LASTST,
       ST_RX_ADV, ST_RX_ARM, ST_RX_IND, ST_RX_DROP, ST_RX_NOTIFY, ST_TX_L2, ST_TX_L2HDR, ST_TX_L3, ST_N };
static volatile LONG g_St[ST_N];
static volatile LONG g_RxL2 = -1;   /* Layer2Type seen on TX packets; RX uses the same */
static volatile LONG g_RxL2Hdr = 0;

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

    case WDI_TASK_CONNECT: {
        WDI_TASK_CONNECT_PARAMETERS p = {};
        NTSTATUS st = Ndis2Nt(ParseWdiTaskConnectToIhv(inLen - sizeof(WDI_MESSAGE_HEADER),
                                buf + sizeof(WDI_MESSAGE_HEADER), &ctx->Tlv, &p));
        if (!NT_SUCCESS(st)) {
            CleanupParsedWdiTaskConnectToIhv(&p);
            WifiRequestComplete(Request, st, sizeof(WDI_MESSAGE_HEADER));
            return;
        }
        if (p.PreferredBSSEntryList.ElementCount == 0 ||
            InterlockedCompareExchange(&ctx->ConnPending, 1, 0) != 0) {
            CleanupParsedWdiTaskConnectToIhv(&p);
            WifiRequestComplete(Request, STATUS_INVALID_DEVICE_STATE, sizeof(WDI_MESSAGE_HEADER));
            return;
        }

        const WDI_CONNECT_BSS_ENTRY_CONTAINER& e = p.PreferredBSSEntryList.pElements[0];
        UCHAR bssid[6], ssid[32], ssidLen = 0, ch = (UCHAR)e.ChannelInfo.ChannelNumber;
        RtlCopyMemory(bssid, e.BSSID.Address, 6);

        /* SSID + channel come from the beacon / probe response body (IEs start after ts+interval+cap = 12 bytes) */
        const UINT8* body = nullptr; ULONG bl = 0;
        if (e.Optional.ProbeResponseFrame_IsPresent) { body = e.ProbeResponseFrame.pElements; bl = e.ProbeResponseFrame.ElementCount; }
        else if (e.Optional.BeaconFrame_IsPresent)   { body = e.BeaconFrame.pElements;        bl = e.BeaconFrame.ElementCount; }
        for (ULONG o = 12; body && o + 2 <= bl; ) {
            UCHAR id = body[o], l = body[o + 1];
            if (o + 2 + l > bl) break;
            if (id == 0 && l <= 32) { RtlCopyMemory(ssid, body + o + 2, l); ssidLen = l; }
            else if (id == 3 && l >= 1 && ch == 0) ch = body[o + 2];
            o += 2 + l;
        }

        ctx->ConnBeaconLen = 0;
        if (body && bl && bl <= sizeof(ctx->ConnBeacon)) {
            RtlCopyMemory(ctx->ConnBeacon, body, bl);
            ctx->ConnBeaconLen = bl;
        }

        UCHAR ext[256]; ULONG extLen = 0;
        if (p.ConnectParameters.Optional.AssociationRequestVendorIE_IsPresent) {
            extLen = p.ConnectParameters.AssociationRequestVendorIE.ElementCount;
            if (extLen > sizeof(ext)) extLen = 0;
            else if (extLen) RtlCopyMemory(ext, p.ConnectParameters.AssociationRequestVendorIE.pElements, extLen);
        }
        ULONG auth = p.ConnectParameters.AuthenticationAlgorithms.ElementCount
                   ? (ULONG)p.ConnectParameters.AuthenticationAlgorithms.pElements[0] : 0;
        WLog(Device, L"Log_Wifi_ConnAuth", auth);
        WLog(Device, L"Log_Wifi_ConnCh", ch);
        WLog(Device, L"Log_Wifi_ConnSsidLen", ssidLen);
        CleanupParsedWdiTaskConnectToIhv(&p);

        ctx->ConnHdr = hdr;
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));      /* M3 */
        st = Rtl_WifiConnect(Device, bssid, ssid, ssidLen, ch, ext, extLen);
        WLog(Device, L"Log_Wifi_ConnStart", (ULONG)st);
        if (!NT_SUCCESS(st)) {
            InterlockedExchange(&ctx->ConnPending, 0);
            SendM4(Device, WDI_INDICATION_CONNECT_COMPLETE, hdr, st);
        } else {
            RtlCopyMemory(ctx->Bssid, bssid, 6);
            ctx->ConnChannel = ch;
        }
        return;
    }

    case WDI_TASK_DISCONNECT:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));      /* M3 */
        ctx->DiscHdr = hdr;
        InterlockedExchange(&ctx->DiscPending, 1);
        Rtl_WifiDisconnect(Device);                       /* M4 comes from WifiCx_OnDisconnectDone */
        return;

    default:
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
/* connect results / link events                                       */
/* ------------------------------------------------------------------ */
extern "C" VOID WifiCx_OnConnectResult(WDFDEVICE Device, NTSTATUS Status, USHORT StatusCode, USHORT Aid,
                                       const UCHAR* Bssid, const UCHAR* AssocReq, ULONG AssocReqLen,
                                       const UCHAR* AssocResp, ULONG AssocRespLen)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    UNREFERENCED_PARAMETER(Aid);
    if (InterlockedCompareExchange(&ctx->ConnPending, 0, 1) != 1) return;

    if (NT_SUCCESS(Status)) {
        /* 1) association result */
        WDI_INDICATION_ASSOCIATION_RESULT_LIST list;
        list.AssociationResults.AllocateElements(1, 0);
        if (list.AssociationResults.ElementCount == 1) {
            WDI_ASSOCIATION_RESULT_CONTAINER& r = list.AssociationResults.pElements[0];
            RtlCopyMemory(r.BSSID.Address, Bssid, 6);
            r.AssociationResultParameters.AssociationStatus = (WDI_ASSOC_STATUS)0;      /* success */
            r.AssociationResultParameters.StatusCode = 0;
            r.AssociationResultParameters.ReAssociation = FALSE;
            r.AssociationResultParameters.AuthAlgorithm = (WDI_AUTH_ALGORITHM)1;        /* 802.11 open */
            r.AssociationResultParameters.UnicastCipherAlgorithm = (WDI_CIPHER_ALGORITHM)0;
            r.AssociationResultParameters.MulticastDataCipherAlgorithm = (WDI_CIPHER_ALGORITHM)0;
            r.AssociationResultParameters.MulticastMgmtCipherAlgorithm = (WDI_CIPHER_ALGORITHM)0;
            r.AssociationResultParameters.PortAuthorized = TRUE;
            r.AssociationResultParameters.BandID = WDI_BAND_ID_2400;
            if (AssocReqLen) {
                r.Optional.AssociationRequestFrame_IsPresent = 1;
                r.AssociationRequestFrame.SimpleAssign(const_cast<UINT8*>(AssocReq), AssocReqLen);
            }
            if (AssocRespLen) {
                r.Optional.AssociationResponseFrame_IsPresent = 1;
                r.AssociationResponseFrame.SimpleAssign(const_cast<UINT8*>(AssocResp), AssocRespLen);
            }
            if (ctx->ConnBeaconLen) {
                r.Optional.BeaconProbeResponse_IsPresent = 1;
                r.BeaconProbeResponse.SimpleAssign(ctx->ConnBeacon, ctx->ConnBeaconLen);
            }
            WDI_PHY_TYPE phys[2] = { (WDI_PHY_TYPE)6 /* ERP */, (WDI_PHY_TYPE)7 /* HT */ };
            r.ActivePhyTypeList.SimpleAssign(phys, 2);
            UINT8* out = nullptr; ULONG cb = 0;
            NDIS_STATUS g = GenerateWdiIndicationAssociationResultFromIhv(&list, 0, &ctx->Tlv, &cb, &out);
            WLog(Device, L"Log_Wifi_AssocResultGen", (ULONG)g);
            if (g == 0) {
                SendIndication(Device, ctx->ConnHdr, WDI_INDICATION_ASSOCIATION_RESULT, 0, STATUS_SUCCESS, out, cb);
                FreeGenerated(out);
            }
        }

        /* 2) link state change */
        WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS ls = {};
        WDI_LINK_INFO_CONTAINER li = {};
        RtlCopyMemory(&ls.LinkStateChangeParameters.PeerMACAddress, Bssid, 6);
        ls.LinkStateChangeParameters.TxLinkSpeed = 11000;
        ls.LinkStateChangeParameters.RxLinkSpeed = 11000;
        ls.LinkStateChangeParameters.LinkQuality = 60;
        li.LinkID = 0;
        RtlCopyMemory(&li.LocalLinkMACAddress, ctx->Mac, 6);
        RtlCopyMemory(&li.PeerLinkMACAddress, Bssid, 6);
        li.ChannelNumber = ctx->ConnChannel;
        li.BandId = WDI_BAND_ID_2400;
        li.RSSI = -50;
        li.Bandwidth = 20;
        ls.LinkInfo.SimpleAssign(&li, 1);
        UINT8* out2 = nullptr; ULONG cb2 = 0;
        NDIS_STATUS g2 = GenerateWdiIndicationLinkStateChangeFromIhv(&ls, 0, &ctx->Tlv, &cb2, &out2);
        WLog(Device, L"Log_Wifi_LinkStateGen", (ULONG)g2);
        if (g2 == 0) {
            SendIndication(Device, ctx->ConnHdr, WDI_INDICATION_LINK_STATE_CHANGE, 0, STATUS_SUCCESS, out2, cb2);
            FreeGenerated(out2);
        }
        ctx->Connected = TRUE;
    } else {
        UNREFERENCED_PARAMETER(StatusCode);
    }
    SendM4(Device, WDI_INDICATION_CONNECT_COMPLETE, ctx->ConnHdr, Status);   /* M4 */
    WLog(Device, L"Log_Wifi_ConnDone", (ULONG)Status);
}

extern "C" VOID WifiCx_OnDisconnectDone(WDFDEVICE Device)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    ctx->Connected = FALSE;
    if (InterlockedCompareExchange(&ctx->DiscPending, 0, 1) == 1)
        SendM4(Device, WDI_INDICATION_DISCONNECT_COMPLETE, ctx->DiscHdr, STATUS_SUCCESS);
}

extern "C" VOID WifiCx_OnLinkLost(WDFDEVICE Device, USHORT Reason)
{
    PWIFI_CTX ctx = GetWifiCtx(Device);
    ctx->Connected = FALSE;
    WLog(Device, L"Log_Wifi_LinkLost", Reason);
    /* TODO(5d): unsolicited WDI disconnect indication so Windows leaves the connected state */
}

/* ------------------------------------------------------------------ */
/* data path: TX (Ethernet -> driver.c queue), RX (ring -> NetRing)    */
/* ------------------------------------------------------------------ */
static VOID EvtTxAdvance(NETPACKETQUEUE q)
{
    PQUEUE_CTX qc = GetQueueCtx(q);
    NET_RING_COLLECTION const* rings = NetTxQueueGetRingCollection(q);
    NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rings);
    InterlockedIncrement(&g_St[ST_TX_ADV]);

    while (NetPacketIteratorHasAny(&pi)) {
        NET_PACKET* pkt = NetPacketIteratorGetPacket(&pi);
        InterlockedIncrement(&g_St[ST_TX_PKT]);
        InterlockedExchange(&g_St[ST_TX_L2], (LONG)pkt->Layout.Layer2Type);
        InterlockedExchange(&g_St[ST_TX_L2HDR], (LONG)pkt->Layout.Layer2HeaderLength);
        InterlockedExchange(&g_St[ST_TX_L3], (LONG)pkt->Layout.Layer3Type);
        InterlockedExchange(&g_RxL2, (LONG)pkt->Layout.Layer2Type);
        InterlockedExchange(&g_RxL2Hdr, (LONG)pkt->Layout.Layer2HeaderLength);
        if (!pkt->Ignore) {
            UCHAR tmp[1536];
            ULONG n = 0;
            BOOLEAN ok = TRUE;
            NET_RING_FRAGMENT_ITERATOR fi = NetPacketIteratorGetFragments(&pi);
            while (NetFragmentIteratorHasAny(&fi)) {
                NET_FRAGMENT* fr = NetFragmentIteratorGetFragment(&fi);
                NET_FRAGMENT_VIRTUAL_ADDRESS const* va =
                    NetExtensionGetFragmentVirtualAddress(&qc->VaExt, NetFragmentIteratorGetIndex(&fi));
                ULONG l = (ULONG)fr->ValidLength;
                if (n + l > sizeof(tmp)) { ok = FALSE; break; }
                RtlCopyMemory(tmp + n, (PUCHAR)va->VirtualAddress + fr->Offset, l);
                n += l;
                NetFragmentIteratorAdvance(&fi);
            }
            if (ok && n >= 14) {
                NTSTATUS ts = Rtl_TxEthernet(g_WifiDevice, tmp, n);
                if (NT_SUCCESS(ts)) InterlockedIncrement(&g_St[ST_TX_SUBMIT_OK]);
                else { InterlockedIncrement(&g_St[ST_TX_SUBMIT_FAIL]); InterlockedExchange(&g_St[ST_TX_LASTST], (LONG)ts); }
            }
        }
        NetPacketIteratorAdvance(&pi);
    }
    NetPacketIteratorSet(&pi);

    NET_RING_FRAGMENT_ITERATOR fi2 = NetRingGetAllFragments(rings);
    NetFragmentIteratorAdvanceToTheEnd(&fi2);
    NetFragmentIteratorSet(&fi2);
}
static VOID EvtTxNotify(NETPACKETQUEUE, BOOLEAN) {}
static VOID EvtTxCancel(NETPACKETQUEUE q)
{
    NET_RING_COLLECTION const* rings = NetTxQueueGetRingCollection(q);
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

static VOID EvtRxAdvance(NETPACKETQUEUE q)
{
    PQUEUE_CTX qc = GetQueueCtx(q);
    PWIFI_CTX w = qc->Wifi;
    NET_RING_COLLECTION const* rings = NetRxQueueGetRingCollection(q);
    InterlockedIncrement(&g_St[ST_RX_ADV]);

    /* hand all buffers the OS posted to us */
    NET_RING_FRAGMENT_ITERATOR post = NetRingGetPostFragments(rings);
    NetFragmentIteratorAdvanceToTheEnd(&post);
    NetFragmentIteratorSet(&post);

    NET_RING_FRAGMENT_ITERATOR fi = NetRingGetDrainFragments(rings);
    NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rings);
    KIRQL irql;

    for (;;) {
        if (!NetFragmentIteratorHasAny(&fi)) break;
        KeAcquireSpinLock(&w->RxLock, &irql);
        if (w->RxHead == w->RxTail) { KeReleaseSpinLock(&w->RxLock, irql); break; }
        ULONG idx = w->RxTail % 64;
        NET_FRAGMENT* fr = NetFragmentIteratorGetFragment(&fi);
        ULONG len = w->RxLen[idx];
        if (len <= fr->Capacity) {
            NET_FRAGMENT_VIRTUAL_ADDRESS const* va =
                NetExtensionGetFragmentVirtualAddress(&qc->VaExt, NetFragmentIteratorGetIndex(&fi));
            RtlCopyMemory((PUCHAR)va->VirtualAddress, w->Rx[idx], len);
            fr->ValidLength = len;
            fr->Offset = 0;
            NET_PACKET* pkt = NetPacketIteratorGetPacket(&pi);
            pkt->FragmentIndex = NetFragmentIteratorGetIndex(&fi);
            pkt->FragmentCount = 1;
            pkt->Layout = {};
            pkt->Layout.Layer2Type = (g_RxL2 >= 0) ? (NET_PACKET_LAYER2_TYPE)g_RxL2 : NetPacketLayer2TypeEthernet;
            pkt->Layout.Layer2HeaderLength = (g_RxL2 >= 0) ? (UINT8)g_RxL2Hdr : 0;
            NetFragmentIteratorAdvance(&fi);
            NetPacketIteratorAdvance(&pi);
            InterlockedIncrement(&g_St[ST_RX_IND]);
        } else {
            w->RxDropped++;
        }
        w->RxTail++;
        KeReleaseSpinLock(&w->RxLock, irql);
    }
    NetFragmentIteratorSet(&fi);
    NetPacketIteratorSet(&pi);
}

static VOID EvtRxNotify(NETPACKETQUEUE q, BOOLEAN enabled)
{
    PQUEUE_CTX qc = GetQueueCtx(q);
    InterlockedExchange(&qc->Wifi->RxNotifyArmed, enabled ? 1 : 0);
    if (enabled) InterlockedIncrement(&g_St[ST_RX_ARM]);
}

static VOID EvtRxCancel(NETPACKETQUEUE q)
{
    PQUEUE_CTX qc = GetQueueCtx(q);
    NET_RING_COLLECTION const* rings = NetRxQueueGetRingCollection(q);
    NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rings);
    while (NetPacketIteratorHasAny(&pi)) {
        NetPacketIteratorGetPacket(&pi)->Ignore = 1;
        NetPacketIteratorAdvance(&pi);
    }
    NetPacketIteratorSet(&pi);
    NET_RING_FRAGMENT_ITERATOR fi = NetRingGetAllFragments(rings);
    NetFragmentIteratorAdvanceToTheEnd(&fi);
    NetFragmentIteratorSet(&fi);
    qc->Wifi->RxQueue = nullptr;
}

static NTSTATUS EvtCreateTxQueue(NETADAPTER, NETTXQUEUE_INIT* Init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE q;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, QUEUE_CTX);
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtTxAdvance, EvtTxNotify, EvtTxCancel);
    NTSTATUS st = NetTxQueueCreate(Init, &attr, &cfg, &q);
    if (!NT_SUCCESS(st)) return st;
    PQUEUE_CTX qc = GetQueueCtx(q);
    qc->Wifi = GetWifiCtx(g_WifiDevice);
    NET_EXTENSION_QUERY query;
    NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(q, &query, &qc->VaExt);
    InterlockedIncrement(&g_St[ST_TXQ_CREATED]);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER, NETRXQUEUE_INIT* Init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE q;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, QUEUE_CTX);
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtRxAdvance, EvtRxNotify, EvtRxCancel);
    NTSTATUS st = NetRxQueueCreate(Init, &attr, &cfg, &q);
    if (!NT_SUCCESS(st)) return st;
    PQUEUE_CTX qc = GetQueueCtx(q);
    qc->Wifi = GetWifiCtx(g_WifiDevice);
    NET_EXTENSION_QUERY query;
    NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(q, &query, &qc->VaExt);
    qc->Wifi->RxQueue = q;
    InterlockedIncrement(&g_St[ST_RXQ_CREATED]);
    return STATUS_SUCCESS;
}

/* called by driver.c at DISPATCH_LEVEL for every received data frame */
extern "C" VOID WifiCx_OnRxData(WDFDEVICE Device, const UCHAR* Da, const UCHAR* Sa, const UCHAR* EtherType,
                                const UCHAR* Payload, ULONG PayloadLen)
{
    PWIFI_CTX w = GetWifiCtx(Device);
    KIRQL irql;
    NETPACKETQUEUE q = nullptr;

    if (PayloadLen + 14 > sizeof(w->Rx[0])) return;
    KeAcquireSpinLock(&w->RxLock, &irql);
    if (w->RxHead - w->RxTail < 64) {
        ULONG idx = w->RxHead % 64;
        UCHAR* d = w->Rx[idx];
        RtlCopyMemory(d, Da, 6);
        RtlCopyMemory(d + 6, Sa, 6);
        d[12] = EtherType[0]; d[13] = EtherType[1];
        RtlCopyMemory(d + 14, Payload, PayloadLen);
        w->RxLen[idx] = (USHORT)(14 + PayloadLen);
        w->RxHead++;
        if (InterlockedExchange(&w->RxNotifyArmed, 0)) q = w->RxQueue;
    } else {
        w->RxDropped++;
    }
    KeReleaseSpinLock(&w->RxLock, irql);
    if (q) { InterlockedIncrement(&g_St[ST_RX_NOTIFY]); NetRxQueueNotifyMoreReceivedPacketsAvailable(q); }
}

extern "C" VOID WifiCx_OnRxFrame(WDFDEVICE Device, const UCHAR* Frame, ULONG Len)
{
    PWIFI_CTX w = GetWifiCtx(Device);
    KIRQL irql;
    NETPACKETQUEUE q = nullptr;

    if (Len > sizeof(w->Rx[0]) || Len < 24) return;
    KeAcquireSpinLock(&w->RxLock, &irql);
    if (w->RxHead - w->RxTail < 64) {
        ULONG idx = w->RxHead % 64;
        RtlCopyMemory(w->Rx[idx], Frame, Len);
        w->RxLen[idx] = (USHORT)Len;
        w->RxHead++;
        if (InterlockedExchange(&w->RxNotifyArmed, 0)) q = w->RxQueue;
    } else {
        w->RxDropped++;
    }
    KeReleaseSpinLock(&w->RxLock, irql);
    if (q) { InterlockedIncrement(&g_St[ST_RX_NOTIFY]); NetRxQueueNotifyMoreReceivedPacketsAvailable(q); }
}

extern "C" VOID WifiCx_GetDataStats(WDFDEVICE Device, ULONG* Out, ULONG Count)
{
    PWIFI_CTX w = GetWifiCtx(Device);
    for (ULONG i = 0; i < Count && i < ST_N; i++) Out[i] = (ULONG)g_St[i];
    if (Count > ST_RX_DROP) Out[ST_RX_DROP] = w->RxDropped;
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
    g_WifiDevice = Device;
    KeInitializeSpinLock(&ctx->RxLock);

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
