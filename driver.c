/*
 * rtl8188eu - Phase 3b (KMDF USB, ARM64)
 *
 * Steps implemented (all verified byte-for-byte against a usbmon capture of
 * the Linux rtl8xxxu driver talking to the same adapter):
 *   1. bind, select config, enumerate bulk pipes
 *   2. EFUSE read + logical-map parse  -> MAC address
 *   3. power-on sequence
 *   4. firmware download (rtl8188eufw.bin rev 28.0) + wait for WINTINI_RDY
 *   5. MAC/BB/AGC/RF register tables replayed from the capture (507 writes) + read-back check
 *   6. RF init / calibration / RX config replayed from the capture (636 writes + delays)
 *   7. passive scan: hop channels 1..13, read bulk-IN, parse beacons/probe responses,
 *      publish SSID list to the registry (proves RF + RX path)
 *
 * Progress/results are written to the device registry key so they can be read
 * without a debugger:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_2357&PID_010C\<serial>\Device Parameters\Log_*
 *
 * Register protocol:
 *   read : bmRequestType 0xC0, bRequest 0x05, wValue = reg, wIndex = 0, wLength = 1..196
 *   write: bmRequestType 0x40, bRequest 0x05, wValue = reg, wIndex = 0, wLength = 1..196
 *   little-endian data.
 */
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <ntstrsafe.h>

#include "wifi.h"
#include "rtl_bss.h"
#include "fwdata.h"
#include "inittab.h"
#include "inittab2.h"
#include "testframe.h"

#define TAG "rtl8188eu: "
#define LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, TAG fmt "\n", __VA_ARGS__)

#define CHK(expr) do { st = (expr); if (!NT_SUCCESS(st)) return st; } while (0)

#define RTL_VENDOR_REQ      0x05
#define REG_SYS_FUNC        0x0002   /* 16-bit, BIT10 = 8051 enable      */
#define REG_APS_FSMCO       0x0004   /* 32-bit, BIT8  = APFM_ONMAC       */
#define REG_EFUSE_CTRL      0x0030
#define REG_MCU_FW_DL       0x0080   /* 32-bit; +2 = page select         */
#define REG_EFUSE_ACCESS    0x00CF
#define REG_SYS_CFG         0x00F0   /* 32-bit; capture showed 0x24403735 */
#define REG_9346CR          0x000A   /* 16-bit; capture showed 0x0020     */

#define MCU_FW_DL_ENABLE    0x01
#define MCU_FW_DL_READY     0x02
#define MCU_FW_DL_CSUM_OK   0x04
#define MCU_WINTINI_RDY     0x40
#define MCU_FW_RAM_SEL      0x80

#define FW_HDR_LEN          32
#define FW_SIG_88E          0x88E1
#define FW_PAGE_SIZE        4096
#define FW_BLOCK            196
#define FW_BASE             0x1000

#define EFUSE_REAL_LEN      512
#define EFUSE_MAP_LEN       512
#define EFUSE_MAC_ADDR_88EU 0xD7

#define MAX_BULK_OUT        3
#define RTL_MAX_BSS         24
#define RX_BUF_SIZE         16384
#define RX_DESC_LEN         24
#define SCAN_DWELL_100NS    3000000LL   /* 300 ms per channel (passive) */
#define ACT_DWELL_100NS     1500000LL   /* 150 ms per channel (active)  */
#define ACT_PROBE2_100NS     700000LL   /* 2nd probe 70 ms into dwell   */

/* Phase 4b: TX. Descriptor is 32 bytes (rtl8xxxu txdesc32 layout, verified on
 * capture: pkt_size 73 + 32 = 105 bytes on bulk OUT ep 0x02). */
#define TXDESC_LEN          32
#define PROBE_LEN           73
#define TX_ENDPOINT         0x02
#define TXBUF_LEN           192   /* desc 32 + up to 24+2+32+47 = 105 frame */
#define EFUSE_TXPWR_CCK     0x10  /* cck_base[6]  : groups 1-2,3-5,6-8,9-11,12-13,14 */
#define EFUSE_TXPWR_HT40    0x16  /* ht40_base[5] : groups 1-2,3-5,6-8,9-11,12-13    */
#define EFUSE_XTAL_K        0xB9
#define REG_AFE_XTAL_CTRL   0x0024
#define TXPWR_DEFAULT       0x2D
#define TX_MAX_CONSEC_FAIL  3
#define SCAN_PASSIVE_MS     300
#define SCAN_ACTIVE_MS      75
#define SCAN_DIRECTED_MS    100
#define TXQ_N               32
#define TXQ_BUF             1600
#define TXD_RATE_DATA       8         /* OFDM 24M default (3 = 11M CCK, 11 = 54M), fixed until TX rate control */
#define JOIN_IDLE           0
#define JOIN_AUTH           1
#define JOIN_ASSOC          2
#define JOIN_UP             3
#define JOB_JOIN            1
#define JOB_LEAVE           2
#define REG_MSR_            0x0102
#define REG_BSSID_          0x0618
#define REG_HMTFR_          0x01CC
#define REG_HMBOX_0_        0x01D0
#define REG_BEACON_CTRL_    0x0550
#define REG_BCN_MAX_ERR_    0x055D
#define REG_BCN_PSR_RPT_    0x06A8
#define REG_SLOT_           0x051B
#define REG_RCR_            0x0608
enum { SCAN_PASSIVE = 1, SCAN_ACTIVE = 2, SCAN_DIRECTED = 3 };



#define KEYQ_N 8
typedef struct _KEY_REQ {
    UCHAR Op;                /* 1 add, 2 delete */
    UCHAR Group, KeyId, HasMac;
    UCHAR Mac[6];
    UCHAR Key[16];
} KEY_REQ;

typedef struct _DEVICE_CONTEXT {
    void           *WdfTriageInfoPtr;       /* MUST be first: NetAdapterCx crash-dump carving */
    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE      BulkIn;
    WDFUSBPIPE      BulkOut[MAX_BULK_OUT];
    ULONG           BulkOutCount;
    WDFUSBPIPE      TxPipe;                 /* bulk OUT ep 0x02 (mgmt queue) */
    UCHAR           Mac[6];
    UCHAR           EfuseMap[EFUSE_MAP_LEN];
    ULONG           ScanFrames;
    ULONG           ScanMgmt;
    ULONG           ScanBadCrc;
    ULONG           ScanProbeResp;
    ULONG           BssCount;
    ULONG           TxSeq;
    ULONG           TxOk;
    ULONG           TxFail;
    NTSTATUS        TxFirstErr;
    UCHAR           TxBuf[TXBUF_LEN];     /* nonpaged context memory */
    ULONG           DirSent;

    /* phase 5a: async engine */
    WDFSPINLOCK     BssLock;                /* protects Bss[]/Scan* counters (RX callback is <= DISPATCH) */
    WDFTIMER        ScanTimer;              /* dwell timer -> enqueues ScanWork */
    WDFWORKITEM     ScanWork;               /* passive-level step: channel hop + TX */
    volatile LONG   HwReady;                /* 1 between D0Entry success and D0Exit */
    volatile LONG   ScanCancel;
    volatile LONG   ScanRunning;
    volatile UCHAR  CurCh;                  /* channel the radio is on (for "heard on") */
    ULONG           ScanPhase, ScanCh, ScanSub, ScanIdx;
    NTSTATUS        ScanStatus;
    ULONG           TxConsecFail;
    ULONG           Snap[3][5];             /* frames, mgmt, badcrc, proberesp, bss after passive/active/directed */
    ULONG           RxCallbacks;
    ULONG           RxZeroLen;
    ULONG64         RxBytes;
    ULONG           ReaderFails;
    ULONG           D0Count, ScanRuns;
    volatile LONG   FastScan;               /* 1 = scan requested by Windows (shorter dwells) */
    /* phase 5d: join + data path */
    WDFDEVICE       Self;
    WDFUSBPIPE      TxPipeData;             /* bulk OUT for BE queue (second out endpoint) */
    WDFWAITLOCK     TxLock;                 /* serialises TxDataBuf users */
    WDFSPINLOCK     TxQLock;
    WDFWORKITEM     JoinWork, LossWork, TxWork, StatsWork, KeyWork;
    /* phase 5d-B: WPA2 / CCMP via the hardware CAM */
    KEY_REQ         KeyQ[KEYQ_N];
    ULONG           KeyQHead, KeyQTail;
    volatile LONG   PtkInstalled;           /* TX encrypts data frames when set */
    BOOLEAN         SecOn;
    ULONG64         TxPn;
    ULONG           RxProt, RxProtDrop, RxProtV0, TxEnc, KeyAdds, KeyDels, KeyLastSt;
    WDFTIMER        StatsTimer;
    KEVENT          JoinEvent;
    volatile LONG   JoinState;
    volatile LONG   Stopping;               /* set at D0Exit: work items must not call into WiFiCx any more */
    volatile LONG   JoinJob;
    UCHAR           JoinBssid[6];
    UCHAR           JoinSsid[32];
    UCHAR           JoinSsidLen;
    UCHAR           JoinCh;
    UCHAR           JoinExtIe[256];
    ULONG           JoinExtIeLen;
    USHORT          JoinAid, JoinStatus, JoinCap, LossReason;
    UCHAR           AssocReq[420];
    ULONG           AssocReqLen;
    UCHAR           AssocResp[420];
    ULONG           AssocRespLen;
    ULONG           HMbox;
    ULONG           TxQHead, TxQTail;
    USHORT          TxQLen[TXQ_N];
    UCHAR           TxQ[TXQ_N][TXQ_BUF];
    UCHAR           TxDataBuf[TXDESC_LEN + TXQ_BUF];
    volatile LONG   TxWorkRunning;
    volatile LONG   TxRate;                 /* Cfg_TxRate: descriptor rate id (3=11M CCK, 11=54M OFDM, 12+=MCS0..) */
    volatile LONG   TxMode;                 /* experiment: Cfg_TxMode registry value, see Tx_Raw */
    ULONG           RxNativeMode, TxNative, RxNative, TxAll, TxCls[5], TxFirstMs, TxLastMs, DbgTxN, DbgRxN; ULONGLONG UpMs; USHORT DbgTxLen[10], DbgRxLen[12]; ULONG DbgRxMs[12], DbgTxMs[10];
    UCHAR           DbgTx[10][96], DbgRx[12][96];
    ULONG           RxDataAny, RxH[48], RxU[48], RxType2, RxFlt[6], RxFirstFc, RxFirstV0, RxFirstV3, RxHdrDbg;
    ULONG           DataTxOk, DataTxFail, DataTxDrop, DataRx, DataRxDrop, JoinRuns;
    BSS_ENTRY       Bss[RTL_MAX_BSS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtDeviceD0Exit;
EVT_WDF_USB_READER_COMPLETION_ROUTINE EvtUsbRxComplete;
EVT_WDF_USB_READERS_FAILED EvtUsbReadersFailed;
EVT_WDF_TIMER EvtScanTimer;
EVT_WDF_WORKITEM EvtScanWork;

/* ---- register access helpers (PASSIVE_LEVEL only) ---------------------- */

static NTSTATUS RtlCtrl(PDEVICE_CONTEXT ctx, BOOLEAN read, USHORT reg,
                        PVOID buf, USHORT len)
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG xfer = 0;
    NTSTATUS st;

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
        &setup,
        read ? BmRequestDeviceToHost : BmRequestHostToDevice,
        BmRequestToDevice,
        RTL_VENDOR_REQ,
        reg,
        0);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, buf, len);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_SEC(1));

    st = WdfUsbTargetDeviceSendControlTransferSynchronously(
            ctx->UsbDevice, WDF_NO_HANDLE, &opts, &setup, &md, &xfer);
    if (NT_SUCCESS(st) && xfer != len) st = STATUS_IO_DEVICE_ERROR;
    return st;
}

static NTSTATUS Rtl_Read8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  *v) { return RtlCtrl(c, TRUE,  r, v, 1); }
static NTSTATUS Rtl_Read16(PDEVICE_CONTEXT c, USHORT r, USHORT *v) { return RtlCtrl(c, TRUE,  r, v, 2); }
static NTSTATUS Rtl_Read32(PDEVICE_CONTEXT c, USHORT r, ULONG  *v) { return RtlCtrl(c, TRUE,  r, v, 4); }
static NTSTATUS Rtl_Write8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  v) { return RtlCtrl(c, FALSE, r, &v, 1); }
static NTSTATUS Rtl_Write16(PDEVICE_CONTEXT c, USHORT r, USHORT v) { return RtlCtrl(c, FALSE, r, &v, 2); }
static NTSTATUS Rtl_Write32(PDEVICE_CONTEXT c, USHORT r, ULONG  v) { return RtlCtrl(c, FALSE, r, &v, 4); }

/* read-modify-write: v = (v & ~clr) | set */
static NTSTATUS Rmw8(PDEVICE_CONTEXT c, USHORT r, UCHAR clr, UCHAR set)
{
    UCHAR v; NTSTATUS st;
    CHK(Rtl_Read8(c, r, &v));
    v = (UCHAR)((v & (UCHAR)~clr) | set);
    return Rtl_Write8(c, r, v);
}
static NTSTATUS Rmw16(PDEVICE_CONTEXT c, USHORT r, USHORT clr, USHORT set)
{
    USHORT v; NTSTATUS st;
    CHK(Rtl_Read16(c, r, &v));
    v = (USHORT)((v & (USHORT)~clr) | set);
    return Rtl_Write16(c, r, v);
}
static NTSTATUS Rmw32(PDEVICE_CONTEXT c, USHORT r, ULONG clr, ULONG set)
{
    ULONG v; NTSTATUS st;
    CHK(Rtl_Read32(c, r, &v));
    v = (v & ~clr) | set;
    return Rtl_Write32(c, r, v);
}

/* ---- registry log: ...\Device Parameters\Log_* -------------------------- */

static VOID RegLog(WDFDEVICE dev, PCWSTR name, ULONG value)
{
    WDFKEY key;
    UNICODE_STRING nm;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    RtlInitUnicodeString(&nm, name);
    (VOID)WdfRegistryAssignULong(key, &nm, value);
    WdfRegistryClose(key);
}

static ULONG RegGetUlong(WDFDEVICE dev, PCWSTR name, ULONG def)
{
    WDFKEY key;
    UNICODE_STRING nm;
    ULONG v = def;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) return def;
    RtlInitUnicodeString(&nm, name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(key, &nm, &v))) v = def;
    WdfRegistryClose(key);
    return v;
}

static VOID RegLogStr(WDFDEVICE dev, PCWSTR name, PCWSTR value)
{
    WDFKEY key;
    UNICODE_STRING nm, val;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    RtlInitUnicodeString(&nm, name);
    RtlInitUnicodeString(&val, value);
    (VOID)WdfRegistryAssignUnicodeString(key, &nm, &val);
    WdfRegistryClose(key);
}

/* ---- EFUSE -------------------------------------------------------------- */

static NTSTATUS EfuseRead8(PDEVICE_CONTEXT c, USHORT addr, UCHAR *out)
{
    NTSTATUS st;
    UCHAR v8;
    ULONG v32 = 0;
    int i;

    CHK(Rtl_Write8(c, REG_EFUSE_CTRL + 1, (UCHAR)(addr & 0xFF)));
    CHK(Rtl_Read8 (c, REG_EFUSE_CTRL + 2, &v8));
    v8 = (UCHAR)((v8 & 0xFC) | ((addr >> 8) & 0x03));
    CHK(Rtl_Write8(c, REG_EFUSE_CTRL + 2, v8));
    CHK(Rtl_Read8 (c, REG_EFUSE_CTRL + 3, &v8));
    CHK(Rtl_Write8(c, REG_EFUSE_CTRL + 3, (UCHAR)(v8 & 0x7F)));   /* clear ready flag */

    for (i = 0; i < 100; i++) {
        CHK(Rtl_Read32(c, REG_EFUSE_CTRL, &v32));
        if (v32 & 0x80000000) { *out = (UCHAR)(v32 & 0xFF); return STATUS_SUCCESS; }
    }
    return STATUS_IO_TIMEOUT;
}

/* Realtek EFUSE format: header byte = (offset<<4 | wordmask) or extended
 * header (low 5 bits 0x0F).  A SET bit in wordmask means the word is NOT
 * present.  Verified against capture: MAC at logical 0xD7 = 10:27:f5:99:50:56 */
static NTSTATUS EfuseParse(PDEVICE_CONTEXT c, ULONG *rawLen)
{
    NTSTATUS st;
    USHORT i = 0;
    UCHAR hdr, h2, mask, b0, b1;
    USHORT offset;
    int w;

    RtlFillMemory(c->EfuseMap, EFUSE_MAP_LEN, 0xFF);

    while (i < EFUSE_REAL_LEN) {
        CHK(EfuseRead8(c, i++, &hdr));
        if (hdr == 0xFF) break;

        if ((hdr & 0x1F) == 0x0F) {
            CHK(EfuseRead8(c, i++, &h2));
            if (h2 == 0xFF) break;
            offset = (USHORT)(((hdr & 0xE0) >> 5) | ((h2 & 0xF0) >> 1));
            mask   = (UCHAR)(h2 & 0x0F);
        } else {
            offset = (USHORT)(hdr >> 4);
            mask   = (UCHAR)(hdr & 0x0F);
        }

        for (w = 0; w < 4; w++) {
            if (!(mask & (1 << w))) {
                CHK(EfuseRead8(c, i++, &b0));
                CHK(EfuseRead8(c, i++, &b1));
                if ((ULONG)offset * 8 + w * 2 + 1 < EFUSE_MAP_LEN) {
                    c->EfuseMap[offset * 8 + w * 2]     = b0;
                    c->EfuseMap[offset * 8 + w * 2 + 1] = b1;
                }
            }
        }
    }
    *rawLen = i;
    return STATUS_SUCCESS;
}

static NTSTATUS Rtl_ReadEfuse(PDEVICE_CONTEXT c, ULONG *rawLen)
{
    NTSTATUS st, st2;
    CHK(Rtl_Write8(c, REG_EFUSE_ACCESS, 0x69));      /* enable */
    st = EfuseParse(c, rawLen);
    st2 = Rtl_Write8(c, REG_EFUSE_ACCESS, 0x00);     /* always disable again */
    return NT_SUCCESS(st) ? st2 : st;
}

/* ---- power on (sequence taken 1:1 from the capture) --------------------- */

static NTSTATUS Rtl_PowerOn(PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    UCHAR t8;
    USHORT t16;
    ULONG t32;
    int i;

    CHK(Rtl_Read8 (c, 0x0100, &t8));
    CHK(Rtl_Read16(c, 0x0008, &t16));
    CHK(Rmw16(c, 0x0004, 0, 0));
    CHK(Rtl_Read32(c, REG_APS_FSMCO, &t32));
    CHK(Rmw8 (c, REG_SYS_FUNC, 0, 0));
    CHK(Rmw32(c, 0x0024, 0, 0x00800000));
    CHK(Rmw16(c, 0x0004, 0, 0));
    CHK(Rmw16(c, 0x0004, 0, 0));
    CHK(Rmw32(c, REG_APS_FSMCO, 0, 0x00000100));      /* APFM_ONMAC: start MAC power-on */

    for (i = 0; i < 500; i++) {                       /* wait for hardware to clear it  */
        CHK(Rtl_Read32(c, REG_APS_FSMCO, &t32));
        if (!(t32 & 0x00000100)) break;
    }
    if (i == 500) return STATUS_IO_TIMEOUT;

    CHK(Rmw8(c, 0x0023, 0, 0));
    CHK(Rtl_Write16(c, 0x0100, 0x063F));              /* CR: enable MAC/DMA blocks */
    CHK(Rtl_Write32(c, 0x0214, 0x0000001C));
    CHK(Rtl_Write32(c, 0x0200, 0x80630029));
    CHK(Rmw16(c, 0x010C, 0, 0xFAF0));
    CHK(Rtl_Write16(c, 0x0116, 0x25FF));
    CHK(Rmw8 (c, 0x0003, 0, 0));
    CHK(Rmw16(c, REG_SYS_FUNC, 0, 0));
    return STATUS_SUCCESS;
}

/* ---- firmware download -------------------------------------------------- */

static NTSTATUS Reset8051(PDEVICE_CONTEXT c)
{
    USHORT v; NTSTATUS st;
    CHK(Rtl_Read16(c, REG_SYS_FUNC, &v));
    CHK(Rtl_Write16(c, REG_SYS_FUNC, (USHORT)(v & ~0x0400)));
    return Rtl_Write16(c, REG_SYS_FUNC, (USHORT)(v | 0x0400));
}

static NTSTATUS Rtl_DownloadFirmware(PDEVICE_CONTEXT c, ULONG *reg80, ULONG *polls)
{
    const UCHAR *fw = g_Rtl8188euFw + FW_HDR_LEN;
    ULONG fwLen = RTL8188EU_FW_LEN - FW_HDR_LEN;
    ULONG page, pages, off, left, v32 = 0;
    USHORT sz;
    UCHAR buf[FW_BLOCK];
    UCHAR v8;
    NTSTATUS st;
    int i;

    /* sanity: header signature + ram code size must match what we ship */
    if ((USHORT)(g_Rtl8188euFw[0] | (g_Rtl8188euFw[1] << 8)) != FW_SIG_88E) return STATUS_INVALID_IMAGE_FORMAT;
    if ((USHORT)(g_Rtl8188euFw[12] | (g_Rtl8188euFw[13] << 8)) != fwLen)    return STATUS_INVALID_IMAGE_FORMAT;

    CHK(Rtl_Read8(c, REG_MCU_FW_DL, &v8));
    if (v8 & MCU_FW_RAM_SEL) {                        /* firmware already running -> reset 8051 */
        CHK(Rtl_Write8(c, REG_MCU_FW_DL, 0x00));
        CHK(Reset8051(c));
    }

    CHK(Rmw8 (c, REG_MCU_FW_DL, 0, MCU_FW_DL_ENABLE));
    CHK(Rmw32(c, REG_MCU_FW_DL, 0x00080000, 0));
    CHK(Rmw8 (c, REG_MCU_FW_DL, 0, MCU_FW_DL_CSUM_OK));  /* reset checksum report */

    pages = (fwLen + FW_PAGE_SIZE - 1) / FW_PAGE_SIZE;
    for (page = 0; page < pages; page++) {
        CHK(Rmw8(c, REG_MCU_FW_DL + 2, 0x07, (UCHAR)page));
        left = fwLen - page * FW_PAGE_SIZE;
        if (left > FW_PAGE_SIZE) left = FW_PAGE_SIZE;
        off = 0;
        while (left) {
            sz = (left >= FW_BLOCK) ? (USHORT)FW_BLOCK : (USHORT)left;
            RtlCopyMemory(buf, fw + page * FW_PAGE_SIZE + off, sz);
            CHK(RtlCtrl(c, FALSE, (USHORT)(FW_BASE + off), buf, sz));
            off += sz;
            left -= sz;
        }
    }

    CHK(Rmw16(c, REG_MCU_FW_DL, MCU_FW_DL_ENABLE, 0));   /* download done */
    CHK(Rtl_Read32(c, REG_MCU_FW_DL, &v32));
    *reg80 = v32;
    if (!(v32 & MCU_FW_DL_CSUM_OK)) return STATUS_DATA_ERROR;   /* checksum failed */

    CHK(Rmw32(c, REG_MCU_FW_DL, MCU_WINTINI_RDY, MCU_FW_DL_READY));
    CHK(Reset8051(c));                                   /* start the firmware */

    for (i = 0; i < 500; i++) {
        CHK(Rtl_Read32(c, REG_MCU_FW_DL, &v32));
        *reg80 = v32;
        *polls = (ULONG)i;
        if (v32 & MCU_WINTINI_RDY) return STATUS_SUCCESS;
    }
    return STATUS_IO_TIMEOUT;
}

/* ---- MAC / baseband / AGC / RF init tables (replayed from capture) ------ */

static NTSTATUS Rtl_ReplayInit(PDEVICE_CONTEXT c, ULONG *done)
{
    ULONG i;
    NTSTATUS st = STATUS_SUCCESS;

    for (i = 0; i < RTL_INIT_COUNT; i++) {
        const RTL_INIT_OP *op = &g_InitTab[i];
        switch (op->Len) {
        case 1:  st = Rtl_Write8 (c, op->Reg, (UCHAR)op->Val);  break;
        case 2:  st = Rtl_Write16(c, op->Reg, (USHORT)op->Val); break;
        default: st = Rtl_Write32(c, op->Reg, op->Val);         break;
        }
        if (!NT_SUCCESS(st)) { *done = i; return st; }
    }
    *done = i;
    return STATUS_SUCCESS;
}

/* Read back every 32-bit baseband register that the table wrote exactly once
 * and compare.  Informational: some bits may legitimately differ. */
static NTSTATUS Rtl_VerifyInit(PDEVICE_CONTEXT c, WDFDEVICE dev, ULONG *mismatch, ULONG *firstReg, ULONG *firstGot)
{
    ULONG i, v;
    NTSTATUS st;
    WCHAR nm[32];

    *mismatch = 0; *firstReg = 0; *firstGot = 0;
    for (i = 0; i < RTL_VERIFY_COUNT; i++) {
        CHK(Rtl_Read32(c, g_VerifyTab[i].Reg, &v));
        if (v != g_VerifyTab[i].Val) {
            if (*mismatch == 0) { *firstReg = g_VerifyTab[i].Reg; *firstGot = v; }
            if (*mismatch < 8) {   /* Log_Mis<n>_Reg / _Got / _Want */
                if (NT_SUCCESS(RtlStringCchPrintfW(nm, 32, L"Log_Mis%u_Reg", *mismatch))) RegLog(dev, nm, g_VerifyTab[i].Reg);
                if (NT_SUCCESS(RtlStringCchPrintfW(nm, 32, L"Log_Mis%u_Got", *mismatch))) RegLog(dev, nm, v);
                if (NT_SUCCESS(RtlStringCchPrintfW(nm, 32, L"Log_Mis%u_Want", *mismatch))) RegLog(dev, nm, g_VerifyTab[i].Val);
            }
            (*mismatch)++;
        }
    }
    return STATUS_SUCCESS;
}

/* ---- phase 3d: RF init / calibration / RX config (capture replay with delays) ---- */

static VOID SleepMs(LONG ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

static NTSTATUS Rtl_ReplayInit2(PDEVICE_CONTEXT c, ULONG *done)
{
    ULONG i;
    NTSTATUS st = STATUS_SUCCESS;

    for (i = 0; i < RTL_INIT2_COUNT; i++) {
        const RTL_INIT_OP *op = &g_Init2Tab[i];
        switch (op->Len) {
        case 0:  SleepMs((LONG)op->Val); st = STATUS_SUCCESS; break;
        case 1:  st = Rtl_Write8 (c, op->Reg, (UCHAR)op->Val);  break;
        case 2:  st = Rtl_Write16(c, op->Reg, (USHORT)op->Val); break;
        default: st = Rtl_Write32(c, op->Reg, op->Val);         break;
        }
        if (!NT_SUCCESS(st)) { *done = i; return st; }
    }
    *done = i;
    return STATUS_SUCCESS;
}

/* RF register 0x18 (channel/bandwidth), path A, via the LSSI write port 0x840:
 * word = (rf_reg << 20) | data.  Capture: ch1 -> 0x01807c01, ch2 -> 0x01807c02 ... */
static NTSTATUS Rtl_SetChannel(PDEVICE_CONTEXT c, UCHAR ch)
{
    return Rtl_Write32(c, 0x0840, 0x01807C00u | ch);
}

/* ---- phase 4a: passive scan over bulk-IN ---------------------------------- */

/* RSSI (dBm) from the RX descriptor PHY status (drvinfo). Formulas follow the Realtek reference
 * driver / rtl8xxxu: CCK uses the AGC report (LNA/VGA index), OFDM/HT uses pwdb_all. */
static LONG Rx_Rssi(const UCHAR *phy, ULONG rate)
{
    LONG pwr;
    if (rate <= 3) {                                   /* CCK 1/2/5.5/11 */
        UCHAR agc = phy[0];
        LONG lna = (agc & 0xE0) >> 5, vga = agc & 0x1F;
        switch (lna) {
        case 7:  pwr = (vga <= 27) ? -100 + 2 * (27 - vga) : -100; break;
        case 6:  pwr = -48 + 2 * (2 - vga); break;
        case 5:  pwr = -42 + 2 * (7 - vga); break;
        case 4:  pwr = -36 + 2 * (7 - vga); break;
        case 3:  pwr = -24 + 2 * (7 - vga); break;
        case 2:  pwr = -12 + 2 * (5 - vga); break;
        case 1:  pwr = 8 - 2 * vga; break;
        default: pwr = 14 - 2 * vga; break;
        }
        pwr += 6;
    } else {
        pwr = (LONG)((phy[1] >> 1) & 0x7F) - 110;
    }
    if (pwr > -10) pwr = -10;
    if (pwr < -100) pwr = -100;
    return pwr;
}

static VOID Scan_AddBss(PDEVICE_CONTEXT c, const UCHAR *bssid, const UCHAR *ssid, UCHAR ssidLen, UCHAR bssCh, UCHAR rxCh,
                        BOOLEAN isResp, const UCHAR *body, ULONG bodyLen, LONG rssi, const UCHAR *phyRaw)
{
    ULONG i;
    BSS_ENTRY *e;
    ULONG keep = 0, o = 0;

    if (ssidLen > 32) ssidLen = 32;

    /* how much of the body we can keep: whole IEs only */
    if (bodyLen >= 12) {
        o = 12;
        while (o + 2 <= bodyLen && o + 2 + body[o + 1] <= bodyLen && o + 2 + body[o + 1] <= BSS_BODY_MAX) o += 2 + body[o + 1];
        keep = o;
    }

    for (i = 0; i < c->BssCount; i++) {
        if (RtlCompareMemory(c->Bss[i].Bssid, bssid, 6) == 6) break;
    }
    if (i == c->BssCount) {
        if (c->BssCount >= RTL_MAX_BSS) return;
        e = &c->Bss[i];
        RtlCopyMemory(e->Bssid, bssid, 6);
        RtlCopyMemory(e->Ssid, ssid, ssidLen);
        e->Ssid[ssidLen] = 0;
        e->Ch = bssCh;
        e->RxCh = rxCh;
        c->BssCount++;
    } else {
        e = &c->Bss[i];
        /* hidden AP: beacon has empty/zeroed SSID, probe response carries the real one */
        if (e->Ssid[0] == 0 && ssidLen > 0 && ssid[0] != 0) {
            RtlCopyMemory(e->Ssid, ssid, ssidLen);
            e->Ssid[ssidLen] = 0;
        }
        if (e->Ch == 0) e->Ch = bssCh;
    }
    e->Hits++;
    if (isResp) e->Resp++;

    /* keep the probe response body in preference to a beacon; otherwise latest frame wins */
    if (keep && (e->BodyLen == 0 || isResp || !e->BodyIsResp)) {
        RtlCopyMemory(e->Body, body, keep);
        e->BodyLen = (USHORT)keep;
        e->BodyIsResp = isResp ? 1 : 0;
    }
    e->Rssi = rssi;
    e->Rate = phyRaw[2]; e->Phy0 = phyRaw[0]; e->Phy1 = phyRaw[1];
    e->Lq = (UCHAR)((rssi <= -90) ? 10 : (rssi >= -40) ? 100 : (10 + (rssi + 90) * 90 / 50));
}

static VOID Scan_ProcessFrame(PDEVICE_CONTEXT c, const UCHAR *f, ULONG len, UCHAR rxCh, LONG rssi, const UCHAR *phyRaw)
{
    UCHAR type, subtype, ch = 0, ssidLen = 0;
    const UCHAR *ssid = (const UCHAR *)"";
    const UCHAR *p, *end;

    c->ScanFrames++;
    if (len < 36) return;
    type = (UCHAR)((f[0] >> 2) & 3);
    subtype = (UCHAR)((f[0] >> 4) & 0xF);
    if (type != 0) return;
    c->ScanMgmt++;
    if (subtype != 8 && subtype != 5) return;          /* beacon / probe response */
    if (subtype == 5) c->ScanProbeResp++;

    p = f + 36;                                        /* 24 hdr + 12 fixed fields */
    end = f + len;
    while (p + 2 <= end) {
        UCHAR id = p[0], l = p[1];
        if (p + 2 + l > end) break;
        if (id == 0) { ssid = p + 2; ssidLen = l; }
        else if (id == 3 && l >= 1) ch = p[2];
        p += 2 + l;
    }
    Scan_AddBss(c, f + 16, ssid, ssidLen, ch, rxCh, (BOOLEAN)(subtype == 5), f + 24, len - 24, rssi, phyRaw);
}

static VOID Scan_ParseBuffer(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len, UCHAR rxCh)
{
    ULONG off = 0;
    while (off + RX_DESC_LEN <= len) {
        ULONG v0 = *(const ULONG *)(buf + off);
        ULONG v3 = *(const ULONG *)(buf + off + 12);
        ULONG pktLen = v0 & 0x3FFF;
        ULONG drvInfo = ((v0 >> 16) & 0xF) * 8;
        ULONG shift = (v0 >> 24) & 3;
        ULONG hdr = RX_DESC_LEN + drvInfo + shift;
        ULONG adv;
        if (pktLen == 0 || off + hdr + pktLen > len) break;
        if (v0 & 0xC000) c->ScanBadCrc++;             /* CRC32 / ICV error */
        else {
            LONG rssi = -80;
            UCHAR raw[3] = {0, 0, 0};
            raw[2] = (UCHAR)(v3 & 0x3F);
            if (drvInfo >= 2 && (v0 & (1u << 26))) {    /* PHY status present */
                raw[0] = buf[off + RX_DESC_LEN]; raw[1] = buf[off + RX_DESC_LEN + 1];
                rssi = Rx_Rssi(buf + off + RX_DESC_LEN, v3 & 0x3F);
            }
            Scan_ProcessFrame(c, buf + off + hdr, pktLen, rxCh, rssi, raw);
        }
        adv = (hdr + pktLen + 127) & ~127u;            /* entries are 128-byte aligned */
        off += adv;
    }
}

/* ---- phase 4b: TX (probe request) + active scan ------------------------- */

/* Probe request exactly as rtl8xxxu sent it in the capture (73 bytes):
 * FC=0x0040, DA=ff*6, SA=our MAC, BSSID=ff*6, SC, SSID(wildcard), rates(1,2,5.5,11,6,9,12,18),
 * ext rates(24,36,48,54), DS channel, HT capabilities. SA/SC/channel patched per frame. */
#define PROBE_OFF_SA   10
#define PROBE_OFF_SC   22
#define PROBE_OFF_CH   44
static const UCHAR g_ProbeTmpl[PROBE_LEN] = {
    0x40, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x10, 0x27, 0xf5, 0x99, 0x50, 0x56,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x04, 0x0b, 0x16,
    0x0c, 0x12, 0x18, 0x24, 0x32, 0x04, 0x30, 0x48, 0x60, 0x6c, 0x03, 0x01, 0x01, 0x2d, 0x1a, 0x6c,
    0x00, 0x1f, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* TX power group, same mapping the capture shows (and rtl8xxxu gen2 uses):
 * ch1-2 ->0, 3-5 ->1, 6-8 ->2, 9-11 ->3, 12-13 ->4, 14 ->5 */
static ULONG Rtl_ChannelToGroup(UCHAR ch)
{
    if (ch < 3) return 0;
    if (ch < 6) return 1;
    if (ch < 9) return 2;
    if (ch < 12) return 3;
    if (ch < 14) return 4;
    return 5;
}

/* cck / ht40(=ofdm,mcs) base index for a channel, from the EFUSE map */
static VOID Rtl_TxPowerForChannel(PDEVICE_CONTEXT c, UCHAR ch, UCHAR *cck, UCHAR *ht)
{
    ULONG g = Rtl_ChannelToGroup(ch);
    UCHAR v = c->EfuseMap[EFUSE_TXPWR_CCK + g];
    UCHAR h = c->EfuseMap[EFUSE_TXPWR_HT40 + (g > 4 ? 4 : g)];
    *cck = (v == 0xFF) ? TXPWR_DEFAULT : v;      /* blank EFUSE -> safe default */
    *ht  = (h == 0xFF) ? TXPWR_DEFAULT : h;
}

/* Per-channel TX power / band regs, same writes the capture did on every hop.
 * Power values now come from EFUSE. 0xE08 low byte (0x2D) and the 0x0390 upper half
 * were constant in the capture and no register read preceded the write, so they stay
 * constants (unverified for other adapters). */
static NTSTATUS Rtl_TxChannelSetup(PDEVICE_CONTEXT c, UCHAR ch)
{
    NTSTATUS st;
    UCHAR cck, ht;
    ULONG pp;

    Rtl_TxPowerForChannel(c, ch, &cck, &ht);
    pp = (ULONG)ht | ((ULONG)ht << 8) | ((ULONG)ht << 16) | ((ULONG)ht << 24);

    CHK(Rtl_Write32(c, 0x0E08, 0x03900000u | ((ULONG)cck << 8) | 0x2D));
    CHK(Rtl_Write32(c, 0x086C, ((ULONG)cck << 8) | ((ULONG)cck << 16) | ((ULONG)cck << 24)));
    CHK(Rtl_Write32(c, 0x0E00, pp));
    CHK(Rtl_Write32(c, 0x0E04, pp));
    CHK(Rtl_Write32(c, 0x0E10, pp));
    CHK(Rtl_Write32(c, 0x0E14, pp));
    CHK(Rtl_Write32(c, 0x0E18, pp));
    CHK(Rtl_Write32(c, 0x0E1C, pp));
    CHK(Rtl_Write8 (c, 0x0603, 0x04));
    CHK(Rtl_Write32(c, 0x0800, 0x83040000u));    /* 2.4 GHz: CCK + OFDM on */
    CHK(Rtl_Write32(c, 0x0900, 0x00000000u));
    return STATUS_SUCCESS;
}

/* Self-check: EFUSE-derived power must equal what the capture wrote for this adapter
 * (cck: ch1-2 0x2f, ch3-8 0x2e, ch9-13 0x2d; ht40: 0x32 / 0x31 / 0x30). */
static ULONG Rtl_TxPowerSelfCheck(PDEVICE_CONTEXT c)
{
    ULONG bad = 0;
    UCHAR ch, cck, ht, wc, wh;
    for (ch = 1; ch <= 13; ch++) {
        wc = (ch <= 2) ? 0x2F : (ch <= 8) ? 0x2E : 0x2D;
        wh = (ch <= 2) ? 0x32 : (ch <= 8) ? 0x31 : 0x30;
        Rtl_TxPowerForChannel(c, ch, &cck, &ht);
        if (cck != wc || ht != wh) bad++;
    }
    return bad;
}

/* Crystal cap -> AFE_XTAL_CTRL bits 11..22 = cap | cap<<6 (capture: cap 0x3f gives 0xfff there) */
static NTSTATUS Rtl_ApplyCrystal(PDEVICE_CONTEXT c, ULONG *capOut, ULONG *regOut)
{
    UCHAR cap = c->EfuseMap[EFUSE_XTAL_K];
    ULONG v;
    NTSTATUS st;
    *capOut = cap;
    if (cap == 0xFF) { *regOut = 0; return STATUS_SUCCESS; }   /* blank EFUSE: keep table value */
    cap &= 0x3F;
    CHK(Rtl_Read32(c, REG_AFE_XTAL_CTRL, &v));
    v &= ~0x007FF800u;
    v |= ((ULONG)cap | ((ULONG)cap << 6)) << 11;
    CHK(Rtl_Write32(c, REG_AFE_XTAL_CTRL, v));
    CHK(Rtl_Read32(c, REG_AFE_XTAL_CTRL, regOut));
    return STATUS_SUCCESS;
}

/* Probe request. ssid==NULL/len 0 -> wildcard (as captured); otherwise directed.
 * frame = 24 byte header + SSID IE + the captured tail (rates, ext rates, DS, HT caps) */
static NTSTATUS Rtl_SendProbe(PDEVICE_CONTEXT c, UCHAR ch, const UCHAR *ssid, UCHAR ssidLen)
{
    ULONG w[8];
    USHORT ck = 0, sc;
    ULONG seq = c->TxSeq++ & 0xFFF;
    PUCHAR b = c->TxBuf, f = c->TxBuf + TXDESC_LEN;
    ULONG fl, tail = PROBE_LEN - 26;
    UCHAR i;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR written = 0;
    NTSTATUS st;

    if (!c->TxPipe) return STATUS_DEVICE_NOT_READY;
    if (ssid == NULL) ssidLen = 0;
    if (ssidLen > 32) ssidLen = 32;

    RtlCopyMemory(f, g_ProbeTmpl, 24);                              /* header */
    if ((c->Mac[0] | c->Mac[1] | c->Mac[2] | c->Mac[3] | c->Mac[4] | c->Mac[5]) != 0)
        RtlCopyMemory(f + PROBE_OFF_SA, c->Mac, 6);
    sc = (USHORT)(seq << 4);
    f[PROBE_OFF_SC]     = (UCHAR)(sc & 0xFF);
    f[PROBE_OFF_SC + 1] = (UCHAR)(sc >> 8);
    f[24] = 0;                                                      /* SSID IE */
    f[25] = ssidLen;
    if (ssidLen) RtlCopyMemory(f + 26, ssid, ssidLen);
    RtlCopyMemory(f + 26 + ssidLen, g_ProbeTmpl + 26, tail);        /* captured IEs */
    f[26 + ssidLen + (PROBE_OFF_CH - 26)] = ch;                     /* DS parameter set */
    fl = 26 + ssidLen + tail;                                       /* 73 for wildcard */

    /* descriptor: values taken 1:1 from the 104 captured probe requests */
    w[0] = 0x8D200000u | fl;          /* OWN|FSG|LSG|BMC, pkt_offset=32, pkt_size */
    w[1] = 0x00001200u;               /* queue select = MGNT (0x12) */
    w[2] = 0x03010000u;
    w[3] = seq << 16;                 /* sequence number */
    w[4] = 0x00000100u;
    w[5] = 0x001A0000u;
    w[6] = 0;
    w[7] = 0x20000000u;               /* checksum goes in the low 16 bits */
    for (i = 0; i < 8; i++) ck ^= (USHORT)(w[i] & 0xFFFF) ^ (USHORT)(w[i] >> 16);
    w[7] |= ck;
    RtlCopyMemory(b, w, TXDESC_LEN);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, b, TXDESC_LEN + fl);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(500));
    st = WdfUsbTargetPipeWriteSynchronously(c->TxPipe, WDF_NO_HANDLE, &opts, &md, (PULONG)&written);
    if (NT_SUCCESS(st) && written != TXDESC_LEN + fl) st = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(st)) c->TxOk++;
    else { if (c->TxFail == 0) c->TxFirstErr = st; c->TxFail++; }
    return st;
}

/* ---- registry dump of the BSS table (passive level) ---------------------- */

static VOID LogBssList(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    ULONG bi, n;
    BSS_ENTRY *snap;   /* copy under lock, registry calls must run unlocked */

    snap = (BSS_ENTRY *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BSS_ENTRY) * RTL_MAX_BSS, 'ssBR');
    if (!snap) return;
    WdfSpinLockAcquire(c->BssLock);
    n = c->BssCount;
    RtlCopyMemory(snap, c->Bss, sizeof(BSS_ENTRY) * n);
    WdfSpinLockRelease(c->BssLock);

    for (bi = 0; bi < n; bi++) {
        WCHAR name[24], val[176], ssw[34];
        ULONG k;
        for (k = 0; snap[bi].Ssid[k] && k < 32; k++)
            ssw[k] = (snap[bi].Ssid[k] >= 0x20 && snap[bi].Ssid[k] < 0x7F) ? (WCHAR)snap[bi].Ssid[k] : L'?';
        ssw[k] = 0;
        if (NT_SUCCESS(RtlStringCchPrintfW(name, 24, L"Scan_Bss%02u", bi)) &&
            NT_SUCCESS(RtlStringCchPrintfW(val, 176, L"%ws | %02x:%02x:%02x:%02x:%02x:%02x | ch%u (heard on %u) | hits %lu | resp %lu | rssi %ld rate %u phy %02x %02x",
                ssw[0] ? ssw : L"<hidden>",
                snap[bi].Bssid[0], snap[bi].Bssid[1], snap[bi].Bssid[2],
                snap[bi].Bssid[3], snap[bi].Bssid[4], snap[bi].Bssid[5],
                (ULONG)snap[bi].Ch, (ULONG)snap[bi].RxCh, snap[bi].Hits, snap[bi].Resp, snap[bi].Rssi, (ULONG)snap[bi].Rate, (ULONG)snap[bi].Phy0, (ULONG)snap[bi].Phy1)))
            RegLogStr(dev, name, val);
    }
    ExFreePoolWithTag(snap, 'ssBR');
}

/* ---- phase 5a: RX via WDF continuous reader ------------------------------ */

static VOID Rx_Dispatch(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len);

VOID EvtUsbRxComplete(WDFUSBPIPE pipe, WDFMEMORY mem, size_t n, WDFCONTEXT ctx)
{
    PDEVICE_CONTEXT c = GetDeviceContext((WDFDEVICE)ctx);
    PUCHAR p;
    UNREFERENCED_PARAMETER(pipe);

    if (n == 0) { c->RxZeroLen++; return; }
    p = (PUCHAR)WdfMemoryGetBuffer(mem, NULL);
    c->RxCallbacks++;
    c->RxBytes += n;

    WdfSpinLockAcquire(c->BssLock);
    Scan_ParseBuffer(c, p, (ULONG)n, c->CurCh);
    WdfSpinLockRelease(c->BssLock);

    if (c->JoinState != JOIN_IDLE) Rx_Dispatch(c, p, (ULONG)n);
}

BOOLEAN EvtUsbReadersFailed(WDFUSBPIPE pipe, NTSTATUS status, USBD_STATUS usbdStatus)
{
    PDEVICE_CONTEXT c = GetDeviceContext(WdfIoTargetGetDevice(WdfUsbTargetPipeGetIoTarget(pipe)));
    UNREFERENCED_PARAMETER(status);
    UNREFERENCED_PARAMETER(usbdStatus);
    c->ReaderFails++;
    return TRUE;            /* reset the pipe and restart the readers */
}

/* ---- phase 5a: scan state machine (timer -> work item, never blocks PnP) -- */

static VOID SnapPhase(PDEVICE_CONTEXT c, ULONG i)
{
    WdfSpinLockAcquire(c->BssLock);
    c->Snap[i][0] = c->ScanFrames;
    c->Snap[i][1] = c->ScanMgmt;
    c->Snap[i][2] = c->ScanBadCrc;
    c->Snap[i][3] = c->ScanProbeResp;
    c->Snap[i][4] = c->BssCount;
    WdfSpinLockRelease(c->BssLock);
}

static NTSTATUS ScanHop(PDEVICE_CONTEXT c, UCHAR ch, BOOLEAN withTxSetup)
{
    NTSTATUS st;
    if (withTxSetup) CHK(Rtl_TxChannelSetup(c, ch));
    CHK(Rtl_SetChannel(c, ch));
    c->CurCh = ch;
    return STATUS_SUCCESS;
}

/* One TX with consecutive-failure tracking. TRUE = TX path still alive. */
static BOOLEAN ScanProbe(PDEVICE_CONTEXT c, UCHAR ch, const UCHAR *ssid, UCHAR n)
{
    if (NT_SUCCESS(Rtl_SendProbe(c, ch, ssid, n))) { c->TxConsecFail = 0; return TRUE; }
    return (++c->TxConsecFail < TX_MAX_CONSEC_FAIL);
}

/* Runs one step. Returns the dwell in ms until the next step, 0 when finished
 * (c->ScanStatus tells whether it finished cleanly). */
static ULONG ScanStep(PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    UCHAR ch;

    for (;;) {
        switch (c->ScanPhase) {

        case SCAN_PASSIVE:
            if (c->ScanCh == 0) {                       /* first step: accept all mgmt frames */
                st = Rtl_Write32(c, 0x0608, 0x7000600E);
                if (!NT_SUCCESS(st)) { c->ScanStatus = st; return 0; }
                c->ScanCh = 1;
            }
            if (c->ScanCh > 13) {
                SnapPhase(c, 0);
                c->ScanPhase = SCAN_ACTIVE; c->ScanCh = 0; c->ScanSub = 0;
                continue;
            }
            st = ScanHop(c, (UCHAR)c->ScanCh, FALSE);
            if (!NT_SUCCESS(st)) { c->ScanStatus = st; return 0; }
            c->ScanCh++;
            return c->FastScan ? 130 : SCAN_PASSIVE_MS;

        case SCAN_ACTIVE:
            if (c->ScanSub == 0) {
                c->ScanCh++;
                if (c->ScanCh > 13) {
                    SnapPhase(c, 1);
                    c->ScanPhase = SCAN_DIRECTED; c->ScanIdx = 0;
                    continue;
                }
                ch = (UCHAR)c->ScanCh;
                st = ScanHop(c, ch, TRUE);
                if (!NT_SUCCESS(st)) { c->ScanStatus = st; return 0; }
                if (!ScanProbe(c, ch, NULL, 0)) { c->ScanStatus = c->TxFirstErr; return 0; }
                c->ScanSub = 1;
            } else {
                if (!ScanProbe(c, (UCHAR)c->ScanCh, NULL, 0)) { c->ScanStatus = c->TxFirstErr; return 0; }
                c->ScanSub = 0;
            }
            return c->FastScan ? 60 : SCAN_ACTIVE_MS;

        case SCAN_DIRECTED:
            while (TRUE) {
                UCHAR ssid[33], n = 0, bch = 0, rxch = 0;
                BOOLEAN have = FALSE;
                RtlZeroMemory(ssid, sizeof(ssid));

                WdfSpinLockAcquire(c->BssLock);
                if (c->ScanIdx < c->BssCount) {
                    RtlCopyMemory(ssid, c->Bss[c->ScanIdx].Ssid, 33);
                    bch = c->Bss[c->ScanIdx].Ch;
                    rxch = c->Bss[c->ScanIdx].RxCh;
                    have = TRUE;
                }
                WdfSpinLockRelease(c->BssLock);

                if (!have) { SnapPhase(c, 2); return 0; }     /* all done, clean finish */
                c->ScanIdx++;
                if (ssid[0] == 0) continue;                  /* hidden: SSID unknown */
                ch = (bch >= 1 && bch <= 13) ? bch : rxch;
                while (n < 32 && ssid[n]) n++;
                st = ScanHop(c, ch, TRUE);
                if (!NT_SUCCESS(st)) { c->ScanStatus = st; return 0; }
                if (NT_SUCCESS(Rtl_SendProbe(c, ch, ssid, n))) c->DirSent++;
                return c->FastScan ? 80 : SCAN_DIRECTED_MS;
            }

        default:
            return 0;
        }
    }
}

static VOID ScanFinish(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    ULONG (*sn)[5] = c->Snap;

    /* passive = snapshot 0; active pass = 1 minus 0; directed pass = 2 minus 1 */
    RegLog(dev, L"Log_Scan_Status", (ULONG)c->ScanStatus);
    RegLog(dev, L"Log_Scan_Frames", sn[0][0]);
    RegLog(dev, L"Log_Scan_Mgmt", sn[0][1]);
    RegLog(dev, L"Log_Scan_BadCrc", sn[0][2]);
    RegLog(dev, L"Log_Scan_BssCount", sn[0][4]);
    RegLog(dev, L"Log_Act_Status", (ULONG)c->ScanStatus);
    RegLog(dev, L"Log_Tx_Ok", c->TxOk);
    RegLog(dev, L"Log_Tx_Fail", c->TxFail);
    RegLog(dev, L"Log_Tx_FirstErr", (ULONG)c->TxFirstErr);
    RegLog(dev, L"Log_Act_Frames", sn[1][0] - sn[0][0]);
    RegLog(dev, L"Log_Act_Mgmt", sn[1][1] - sn[0][1]);
    RegLog(dev, L"Log_Act_BadCrc", sn[1][2] - sn[0][2]);
    RegLog(dev, L"Log_Act_ProbeResp", sn[1][3]);
    RegLog(dev, L"Log_Act_NewBss", sn[1][4] - sn[0][4]);
    RegLog(dev, L"Log_Act_BssCount", sn[1][4]);
    RegLog(dev, L"Log_Dir_Sent", c->DirSent);
    RegLog(dev, L"Log_Dir_Resp", sn[2][3] - sn[1][3]);
    RegLog(dev, L"Log_Rx_Callbacks", c->RxCallbacks);
    RegLog(dev, L"Log_Rx_Bytes", (ULONG)c->RxBytes);
    RegLog(dev, L"Log_Rx_ReaderFails", c->ReaderFails);
    RegLog(dev, L"Log_Rx_ZeroLen", c->RxZeroLen);
    RegLog(dev, L"Log_Rx_StateEnd", (ULONG)WdfIoTargetGetState(WdfUsbTargetPipeGetIoTarget(c->BulkIn)));
    RegLog(dev, L"Log_Scan_Runs", c->ScanRuns);
    LogBssList(dev, c);
    LOG("scan done st=0x%08x tx ok=%lu fail=%lu probe_resp=%lu", c->ScanStatus, c->TxOk, c->TxFail, sn[2][3]);
    if (NT_SUCCESS(c->ScanStatus)) RegLog(dev, L"Log_Stage", 13);   /* all scan phases finished */
    c->FastScan = 0;
    WifiCx_OnScanComplete(dev);          /* answers a pending WDI scan task (if any) */
    InterlockedExchange(&c->ScanRunning, 0);
}

VOID EvtScanTimer(WDFTIMER timer)
{
    PDEVICE_CONTEXT c = GetDeviceContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    WdfWorkItemEnqueue(c->ScanWork);
}

VOID EvtScanWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    ULONG delay;

    if (c->ScanCancel || !c->HwReady) { WifiCx_OnScanComplete(dev); InterlockedExchange(&c->ScanRunning, 0); return; }
    delay = ScanStep(c);
    if (delay == 0) { ScanFinish(dev, c); return; }
    if (c->ScanCancel) { WifiCx_OnScanComplete(dev); InterlockedExchange(&c->ScanRunning, 0); return; }
    WdfTimerStart(c->ScanTimer, WDF_REL_TIMEOUT_IN_MS(delay));
}

static VOID ScanStart(PDEVICE_CONTEXT c)
{
    if (InterlockedCompareExchange(&c->ScanRunning, 1, 0) != 0) return;   /* already running */

    WdfSpinLockAcquire(c->BssLock);
    RtlZeroMemory(c->Bss, sizeof(c->Bss));
    c->BssCount = c->ScanFrames = c->ScanMgmt = c->ScanBadCrc = c->ScanProbeResp = 0;
    WdfSpinLockRelease(c->BssLock);

    RtlZeroMemory(c->Snap, sizeof(c->Snap));
    c->TxOk = c->TxFail = c->DirSent = c->TxConsecFail = 0;
    c->TxFirstErr = STATUS_SUCCESS;
    c->ScanStatus = STATUS_SUCCESS;
    c->ScanPhase = SCAN_PASSIVE;
    c->ScanCh = c->ScanSub = c->ScanIdx = 0;
    c->ScanRuns++;
    InterlockedExchange(&c->ScanCancel, 0);
    WdfWorkItemEnqueue(c->ScanWork);
}

/* ---- exported to wifi.cpp ------------------------------------------------ */

NTSTATUS Rtl_WifiScanRequest(WDFDEVICE dev)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    if (!c->HwReady) return STATUS_DEVICE_NOT_READY;
    if (c->JoinState != 0) return STATUS_DEVICE_BUSY;    /* connected/joining: answer from the cached BSS list */
    c->FastScan = 1;
    ScanStart(c);                                /* joins a scan that is already running */
    return STATUS_SUCCESS;
}

ULONG Rtl_SnapshotBss(WDFDEVICE dev, BSS_ENTRY *out, ULONG max)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    ULONG n;
    WdfSpinLockAcquire(c->BssLock);
    n = c->BssCount < max ? c->BssCount : max;
    RtlCopyMemory(out, c->Bss, sizeof(BSS_ENTRY) * n);
    WdfSpinLockRelease(c->BssLock);
    return n;
}

/* ======================================================================
 * phase 5d: join (open auth + assoc), data TX/RX conversion
 * ====================================================================== */

static USHORT Le16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }

/* Generic TX through the descriptor. mgmt -> ep 0x02 / queue MGNT, data -> second endpoint / queue BE.
 * Called at PASSIVE_LEVEL only. frame = full 802.11 frame (seq ctrl is patched here). */
static NTSTATUS Tx_Raw(PDEVICE_CONTEXT c, const UCHAR *frame, ULONG fl, BOOLEAN mgmt, BOOLEAN bmc)
{
    ULONG w[8];
    USHORT ck = 0;
    ULONG seq;
    UCHAR i;
    PUCHAR b = c->TxDataBuf, f = c->TxDataBuf + TXDESC_LEN;
    LONG mode = mgmt ? 0 : c->TxMode;
    ULONG queue = 0, rate = (ULONG)c->TxRate & 0x7F;
    BOOLEAN useMgmtPipe = FALSE, qos = (BOOLEAN)(frame[0] == 0x88), enc = FALSE;
    WDFUSBPIPE pipe;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR written = 0;
    NTSTATUS st;

    switch (mode) {
    case 1: queue = 0x12; useMgmtPipe = TRUE; break;      /* data frames through the MGNT queue / ep 0x02 */
    case 3: rate = 0; break;                               /* 1M CCK */
    case 4: queue = 0x11; useMgmtPipe = TRUE; break;      /* HIGH queue / ep 0x02 */
    case 5: rate = 4; break;                               /* 6M OFDM */
    case 6: queue = 0x00; useMgmtPipe = TRUE; break;      /* BE queue but through ep 0x02 */
    case 7: queue = 0x12; rate = 0; useMgmtPipe = TRUE; break;
    default: break;                                        /* 0, 2: BE / ep 0x03 / 11M (2 = QoS header, built by the producer) */
    }
    pipe = (mgmt || useMgmtPipe) ? c->TxPipe : (c->TxPipeData ? c->TxPipeData : c->TxPipe);
    if (!pipe || fl < 24 || fl > TXQ_BUF) return STATUS_DEVICE_NOT_READY;

    WdfWaitLockAcquire(c->TxLock, NULL);
    seq = (ULONG)InterlockedIncrement((volatile LONG *)&c->TxSeq) & 0xFFF;
    {
        /* WPA2: protect unicast/ToDS data frames (not EAPOL) with CCMP. We add the 8 byte CCMP header,
         * the hardware encrypts and appends the MIC (length in the descriptor excludes the MIC). */
        ULONG hl = (frame[0] & 0x80) ? 26 : 24;
        if (!mgmt && c->PtkInstalled && (frame[0] & 0x0C) == 0x08 && fl > hl + 8 && fl + 8 <= TXQ_BUF &&
            !(frame[hl + 6] == 0x88 && frame[hl + 7] == 0x8E)) {
            ULONG64 pn = c->TxPn++;
            RtlCopyMemory(f, frame, hl);
            f[1] |= 0x40;                                   /* Protected */
            f[hl + 0] = (UCHAR)pn;         f[hl + 1] = (UCHAR)(pn >> 8);
            f[hl + 2] = 0;                 f[hl + 3] = 0x20;  /* ExtIV, key id 0 */
            f[hl + 4] = (UCHAR)(pn >> 16); f[hl + 5] = (UCHAR)(pn >> 24);
            f[hl + 6] = (UCHAR)(pn >> 32); f[hl + 7] = (UCHAR)(pn >> 40);
            RtlCopyMemory(f + hl + 8, frame + hl, fl - hl);
            fl += 8;
            enc = TRUE;
            c->TxEnc++;
        } else {
            RtlCopyMemory(f, frame, fl);
        }
    }
    f[22] = (UCHAR)((seq << 4) & 0xFF);
    f[23] = (UCHAR)((seq << 4) >> 8);

    w[0] = 0x8C200000u | (bmc ? 0x01000000u : 0) | fl;
    w[1] = (mgmt ? 0x00001200u : (queue << 8)) | (enc ? 0x00C00000u : 0);
    w[2] = 0x03010000u;
    w[3] = seq << 16;
    w[4] = 0x00000100u | (qos ? 0x40u : 0u);
    w[5] = mgmt ? 0x001A0000u : (queue == 0x12 ? (0x001A0000u | rate) : (0x0001FF00u | rate));
    w[6] = 0;
    w[7] = 0x20000000u;
    for (i = 0; i < 8; i++) ck ^= (USHORT)(w[i] & 0xFFFF) ^ (USHORT)(w[i] >> 16);
    w[7] |= ck;
    RtlCopyMemory(b, w, TXDESC_LEN);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, b, TXDESC_LEN + fl);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(500));
    st = WdfUsbTargetPipeWriteSynchronously(pipe, WDF_NO_HANDLE, &opts, &md, (PULONG)&written);
    if (NT_SUCCESS(st) && written != TXDESC_LEN + fl) st = STATUS_IO_DEVICE_ERROR;
    WdfWaitLockRelease(c->TxLock);
    if (mgmt) { if (NT_SUCCESS(st)) c->TxOk++; else c->TxFail++; }
    else { if (NT_SUCCESS(st)) c->DataTxOk++; else c->DataTxFail++; }
    return st;
}

static VOID Mgmt_Header(PDEVICE_CONTEXT c, UCHAR *f, UCHAR fc0)
{
    RtlZeroMemory(f, 24);
    f[0] = fc0;
    RtlCopyMemory(f + 4, c->JoinBssid, 6);     /* A1 = DA = AP */
    RtlCopyMemory(f + 10, c->Mac, 6);          /* A2 = SA = us */
    RtlCopyMemory(f + 16, c->JoinBssid, 6);    /* A3 = BSSID  */
}

static NTSTATUS Join_SendAuth(PDEVICE_CONTEXT c)
{
    UCHAR f[30];
    Mgmt_Header(c, f, 0xB0);
    f[24] = 0; f[25] = 0;      /* open system */
    f[26] = 1; f[27] = 0;      /* seq 1       */
    f[28] = 0; f[29] = 0;      /* status      */
    return Tx_Raw(c, f, sizeof(f), TRUE, FALSE);
}

/* Builds the association request into c->AssocReq (body only, after the 24-byte header, is what WDI wants). */
static ULONG Join_BuildAssocReq(PDEVICE_CONTEXT c, UCHAR *f)
{
    ULONG n = 24;
    Mgmt_Header(c, f, 0x00);
    f[n++] = 0x21; f[n++] = 0x04;                 /* capability: ESS | short preamble | short slot */
    f[n++] = 10;   f[n++] = 0;                    /* listen interval */
    f[n++] = 0;    f[n++] = c->JoinSsidLen;       /* SSID */
    RtlCopyMemory(f + n, c->JoinSsid, c->JoinSsidLen); n += c->JoinSsidLen;
    f[n++] = 1; f[n++] = 8;                       /* supported rates: 1,2,5.5,11 basic + 6,9,12,18 */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;
    f[n++] = 50; f[n++] = 4;                      /* extended rates: 24,36,48,54 */
    f[n++] = 0x30; f[n++] = 0x48; f[n++] = 0x60; f[n++] = 0x6C;
    {   /* HT capabilities exactly as rtl8xxxu sends them (20 MHz, SGI20, MCS0-7): lets the AP use 11n rates towards us */
        RtlCopyMemory(f + n, g_ProbeTmpl + PROBE_LEN - 28, 28); n += 28;   /* tail of the probe template = HT cap IE */
    }
    if (c->JoinExtIeLen && n + c->JoinExtIeLen < 400) {   /* RSN / vendor IEs from Windows */
        RtlCopyMemory(f + n, c->JoinExtIe, c->JoinExtIeLen);
        n += c->JoinExtIeLen;
    }
    return n;
}

static NTSTATUS Join_Wait(PDEVICE_CONTEXT c, ULONG ms)
{
    if (c->Stopping) return STATUS_CANCELLED;
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000LL;
    return KeWaitForSingleObject(&c->JoinEvent, Executive, KernelMode, FALSE, &t);
}

/* H2C media status report (gen2 firmware, 8188e): cmd 0x01, parm = connect | role << 4, macid. */
static NTSTATUS Rtl_H2cMediaStatus(PDEVICE_CONTEXT c, BOOLEAN connect, UCHAR role, UCHAR macid)
{
    ULONG mbox = c->HMbox & 3, retry;
    UCHAR v = 0;
    ULONG msg;
    NTSTATUS st;

    for (retry = 0; retry < 100; retry++) {
        st = Rtl_Read8(c, REG_HMTFR_, &v);
        if (!NT_SUCCESS(st)) return st;
        if (!(v & (1u << mbox))) break;
    }
    if (retry == 100) return STATUS_DEVICE_BUSY;
    msg = 0x01u | ((ULONG)((connect ? 1 : 0) | (role << 4)) << 8) | ((ULONG)macid << 16);
    st = Rtl_Write32(c, (USHORT)(REG_HMBOX_0_ + mbox * 4), msg);
    c->HMbox++;
    return st;
}


/* ---- phase 5d-B: security CAM (layout from the Linux rtl8xxxu capture) ---- */
#define REG_CAMCMD_   0x0670
#define REG_CAMW_     0x0674
#define REG_SECCFG_   0x0680

static NTSTATUS Cam_WriteEntry(PDEVICE_CONTEXT c, ULONG entry, BOOLEAN group, UCHAR keyId, const UCHAR *mac, const UCHAR *key)
{
    LONG j;
    NTSTATUS st;
    ULONG ctrl = (4u << 2) | (keyId & 3) | 0x8000u | (group ? 0x40u : 0);       /* cipher index 4 = CCMP */
    for (j = 5; j >= 0; j--) {
        ULONG v;
        if (j == 0) v = ctrl | ((ULONG)mac[0] << 16) | ((ULONG)mac[1] << 24);
        else if (j == 1) v = mac[2] | ((ULONG)mac[3] << 8) | ((ULONG)mac[4] << 16) | ((ULONG)mac[5] << 24);
        else {
            const UCHAR *k = key + ((j - 2) << 2);
            v = k[0] | ((ULONG)k[1] << 8) | ((ULONG)k[2] << 16) | ((ULONG)k[3] << 24);
        }
        CHK(Rtl_Write32(c, REG_CAMW_, v));
        CHK(Rtl_Write32(c, REG_CAMCMD_, 0x80010000u | (entry << 3) | (ULONG)j));
        KeStallExecutionProcessor(100);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Cam_Invalidate(PDEVICE_CONTEXT c, ULONG entry)
{
    NTSTATUS st;
    CHK(Rtl_Write32(c, REG_CAMW_, 0));
    CHK(Rtl_Write32(c, REG_CAMCMD_, 0x80010000u | (entry << 3)));
    KeStallExecutionProcessor(100);
    return STATUS_SUCCESS;
}

static NTSTATUS Sec_Enable(PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    if (c->SecOn) return STATUS_SUCCESS;
    CHK(Rmw16(c, 0x0100, 0, 0x0200));                         /* CR: security enable */
    CHK(Rtl_Write8(c, REG_SECCFG_, 0xCF));
    c->SecOn = TRUE;
    return STATUS_SUCCESS;
}

static VOID Sec_Reset(PDEVICE_CONTEXT c)
{
    ULONG e;
    InterlockedExchange(&c->PtkInstalled, 0);
    c->TxPn = 1;
    if (c->SecOn) {
        for (e = 0; e < 5; e++) (VOID)Cam_Invalidate(c, e);
        (VOID)Rtl_Write8(c, REG_SECCFG_, 0);
        c->SecOn = FALSE;
    }
}

static VOID EvtKeyWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    for (;;) {
        KEY_REQ r;
        NTSTATUS st = STATUS_SUCCESS;
        ULONG entry;
        const UCHAR *mac;
        WdfSpinLockAcquire(c->TxQLock);
        if (c->KeyQHead == c->KeyQTail) { WdfSpinLockRelease(c->TxQLock); break; }
        r = c->KeyQ[c->KeyQTail % KEYQ_N];
        c->KeyQTail++;
        WdfSpinLockRelease(c->TxQLock);
        if (c->Stopping || !c->HwReady) continue;
        mac = r.HasMac ? r.Mac : c->JoinBssid;
        if (r.Group) entry = (r.KeyId >= 1 && r.KeyId <= 3) ? r.KeyId : 4;
        else entry = 0;
        if (r.Op == 1) {
            st = Sec_Enable(c);
            if (NT_SUCCESS(st)) st = Cam_WriteEntry(c, entry, r.Group, r.Group ? r.KeyId : 0, mac, r.Key);
            if (NT_SUCCESS(st) && !r.Group) {
                c->TxPn = 1;
                InterlockedExchange(&c->PtkInstalled, 1);
            }
            c->KeyAdds++;
        } else {
            st = Cam_Invalidate(c, entry);
            if (!r.Group) InterlockedExchange(&c->PtkInstalled, 0);
            c->KeyDels++;
        }
        c->KeyLastSt = (ULONG)st;
        RegLog(dev, L"Log_Key_Adds", c->KeyAdds);
        RegLog(dev, L"Log_Key_Dels", c->KeyDels);
        RegLog(dev, L"Log_Key_LastSt", c->KeyLastSt);
        RegLog(dev, L"Log_Key_LastGroup", r.Group);
        RegLog(dev, L"Log_Key_LastId", r.KeyId);
        RegLog(dev, L"Log_Key_Ptk", (ULONG)c->PtkInstalled);
    }
}

NTSTATUS Rtl_WifiKey(WDFDEVICE dev, ULONG op, BOOLEAN group, UCHAR keyId, const UCHAR *mac, const UCHAR *key16)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    KEY_REQ *r;
    WdfSpinLockAcquire(c->TxQLock);
    if (c->KeyQHead - c->KeyQTail >= KEYQ_N) { WdfSpinLockRelease(c->TxQLock); return STATUS_INSUFFICIENT_RESOURCES; }
    r = &c->KeyQ[c->KeyQHead % KEYQ_N];
    RtlZeroMemory(r, sizeof(*r));
    r->Op = (UCHAR)op; r->Group = group ? 1 : 0; r->KeyId = keyId;
    if (mac) { r->HasMac = 1; RtlCopyMemory(r->Mac, mac, 6); }
    if (key16) RtlCopyMemory(r->Key, key16, 16);
    c->KeyQHead++;
    WdfSpinLockRelease(c->TxQLock);
    WdfWorkItemEnqueue(c->KeyWork);
    return STATUS_SUCCESS;
}

static NTSTATUS Join_HwUp(PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    UCHAR i;
    InterlockedExchange(&c->PtkInstalled, 0); c->TxPn = 1;
    for (i = 0; i < 6; i++) CHK(Rtl_Write8(c, (USHORT)(REG_BSSID_ + i), c->JoinBssid[i]));
    CHK(Rmw8(c, REG_MSR_, 0x03, 0x02));                         /* link type: station */
    CHK(Rtl_Write8(c, REG_BCN_MAX_ERR_, 0xFF));
    CHK(Rtl_Write16(c, REG_BCN_PSR_RPT_, (USHORT)(0xC000 | c->JoinAid)));
    CHK(Rmw8(c, REG_BEACON_CTRL_, 0x10, 0));                    /* allow TSF update from the AP */
    CHK(Rtl_Write8(c, REG_SLOT_, (c->JoinCap & 0x0400) ? 9 : 20));
    CHK(Rmw32(c, REG_RCR_, 0, 0xCE60087Eu));                    /* APM | AM | AB | accept data frames */
    (VOID)Rtl_H2cMediaStatus(c, TRUE, 2 /* AP */, 0);

    /* --- values seen in the Linux rtl8xxxu capture right after association --- */
    (VOID)Rtl_Write32(c, 0x0440, 0x0008015Fu);                   /* RRSR: response rate set       */
    (VOID)Rtl_Write8(c, 0x0480, 0x03);                          /* INIRTS_RATE_SEL               */
    (VOID)Rmw8(c, 0x0422, 0x40, 0);                       /* stop TX beacon queue download */
    (VOID)Rtl_Write8(c, 0x0541, 0x64);                          /* TBTT prohibit                 */
    (VOID)Rmw8(c, 0x0542, 0x01, 0);
    (VOID)Rtl_Write32(c, 0x0500, 0x002F3222u);                   /* EDCA VO */
    (VOID)Rtl_Write32(c, 0x0504, 0x005E4322u);                   /* EDCA VI */
    (VOID)Rtl_Write32(c, 0x0508, 0x0000A42Bu);                   /* EDCA BE */
    (VOID)Rtl_Write32(c, 0x050C, 0x0000A44Fu);                   /* EDCA BK */
    (VOID)Rtl_Write8(c, 0x0458, 0x41);
    (VOID)Rtl_Write8(c, 0x0459, 0xA8);
    (VOID)Rtl_Write8(c, 0x045A, 0x72);
    (VOID)Rtl_Write8(c, 0x045B, 0xB9);
    (VOID)Rtl_Write8(c, 0x045C, 0x04);
    return STATUS_SUCCESS;
}

static VOID Join_HwDown(PDEVICE_CONTEXT c)
{
    Sec_Reset(c);
    (VOID)Rtl_H2cMediaStatus(c, FALSE, 2, 0);
    (VOID)Rmw8(c, REG_BEACON_CTRL_, 0, 0x10);
    (VOID)Rmw8(c, REG_MSR_, 0x03, 0x00);
}

static VOID Join_StopScan(PDEVICE_CONTEXT c)
{
    InterlockedExchange(&c->ScanCancel, 1);
    WdfTimerStop(c->ScanTimer, TRUE);
    WdfWorkItemFlush(c->ScanWork);
    WdfTimerStop(c->ScanTimer, TRUE);
    WdfWorkItemFlush(c->ScanWork);
    InterlockedExchange(&c->ScanRunning, 0);
}


/* ---- self test: replay the DHCP DISCOVER captured from Linux with the exact Linux descriptor ---- */
static NTSTATUS Tx_TestFrame(PDEVICE_CONTEXT c, ULONG w4, ULONG w5, ULONG w2)
{
    ULONG w[8], i, fl = (ULONG)sizeof(g_TestFrame), seq;
    USHORT ck = 0;
    PUCHAR b = c->TxDataBuf, f = c->TxDataBuf + TXDESC_LEN;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR written = 0;
    NTSTATUS st;
    WDFUSBPIPE pipe = c->TxPipeData ? c->TxPipeData : c->TxPipe;
    if (!pipe) return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(c->TxLock, NULL);
    seq = (ULONG)InterlockedIncrement((volatile LONG *)&c->TxSeq) & 0xFFF;
    RtlCopyMemory(f, g_TestFrame, fl);
    RtlCopyMemory(f + 4, c->JoinBssid, 6);
    RtlCopyMemory(f + 10, c->Mac, 6);
    f[22] = (UCHAR)((seq << 4) & 0xFF); f[23] = (UCHAR)((seq << 4) >> 8);
    w[0] = 0x8D200000u | fl;  w[1] = 0;  w[2] = w2;  w[3] = seq << 16;
    w[4] = w4;  w[5] = w5;  w[6] = 0;  w[7] = 0x20000000u;
    for (i = 0; i < 8; i++) ck ^= (USHORT)(w[i] & 0xFFFF) ^ (USHORT)(w[i] >> 16);
    w[7] |= ck;
    RtlCopyMemory(b, w, TXDESC_LEN);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, b, TXDESC_LEN + fl);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(500));
    st = WdfUsbTargetPipeWriteSynchronously(pipe, WDF_NO_HANDLE, &opts, &md, (PULONG)&written);
    WdfWaitLockRelease(c->TxLock);
    return st;
}

static VOID Join_SelfTest(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    ULONG before, k;
    if (!RegGetUlong(dev, L"Cfg_SelfTest", 0)) return;
    SleepMs(300);
    before = c->RxDataAny;
    for (k = 0; k < 3; k++) { (VOID)Tx_TestFrame(c, 0x0102B148u, 0x0001FF13u, 0x03410000u); SleepMs(150); }
    RegLog(dev, L"Log_Test_A_LinuxDesc_RxData", c->RxDataAny - before);
    before = c->RxDataAny;
    for (k = 0; k < 3; k++) { (VOID)Tx_TestFrame(c, 0x00000148u, 0x0001FF03u, 0x03010000u); SleepMs(150); }
    RegLog(dev, L"Log_Test_B_OurDesc11M_RxData", c->RxDataAny - before);
    before = c->RxDataAny;
    for (k = 0; k < 3; k++) { (VOID)Tx_TestFrame(c, 0x0102B148u, 0x0001FF04u, 0x03410000u); SleepMs(150); }
    RegLog(dev, L"Log_Test_C_LinuxDesc6M_RxData", c->RxDataAny - before);
}

static NTSTATUS Join_Run(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    NTSTATUS st;
    ULONG tries, n;
    UCHAR req[440];

    c->TxMode = (LONG)RegGetUlong(dev, L"Cfg_TxMode", 0);
    c->TxRate = (LONG)RegGetUlong(dev, L"Cfg_TxRate", TXD_RATE_DATA);
    c->RxNativeMode = RegGetUlong(dev, L"Cfg_RxNative", 1);
    Join_StopScan(c);
    st = ScanHop(c, c->JoinCh, TRUE);
    if (!NT_SUCCESS(st)) return st;
    SleepMs(5);

    /* --- authentication --- */
    c->JoinStatus = 0xFFFF;
    for (tries = 0; tries < 4; tries++) {
        KeClearEvent(&c->JoinEvent);
        InterlockedExchange(&c->JoinState, JOIN_AUTH);
        (VOID)Join_SendAuth(c);
        if (Join_Wait(c, 150) == STATUS_SUCCESS) break;
    }
    RegLog(dev, L"Log_Join_AuthTries", tries + 1);
    if (tries == 4) return STATUS_IO_TIMEOUT;
    RegLog(dev, L"Log_Join_AuthStatus", c->JoinStatus);
    if (c->JoinStatus != 0) return STATUS_ACCESS_DENIED;

    /* --- association --- */
    n = Join_BuildAssocReq(c, req);
    c->JoinStatus = 0xFFFF;
    for (tries = 0; tries < 4; tries++) {
        KeClearEvent(&c->JoinEvent);
        InterlockedExchange(&c->JoinState, JOIN_ASSOC);
        (VOID)Tx_Raw(c, req, n, TRUE, FALSE);
        if (Join_Wait(c, 300) == STATUS_SUCCESS) break;
    }
    RegLog(dev, L"Log_Join_AssocTries", tries + 1);
    if (tries == 4) return STATUS_IO_TIMEOUT;
    RegLog(dev, L"Log_Join_AssocStatus", c->JoinStatus);
    if (c->JoinStatus != 0) return STATUS_ACCESS_DENIED;

    c->AssocReqLen = n - 24;
    RtlCopyMemory(c->AssocReq, req + 24, c->AssocReqLen);
    RegLog(dev, L"Log_Join_Aid", c->JoinAid);

    st = Join_HwUp(c);
    RegLog(dev, L"Log_Join_HwUp", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;
    c->UpMs = KeQueryInterruptTime() / 10000;
    c->DbgTxN = c->DbgRxN = 0; c->TxAll = 0; RtlZeroMemory(c->TxCls, sizeof(c->TxCls)); c->TxFirstMs = c->TxLastMs = 0;
    InterlockedExchange(&c->JoinState, JOIN_UP);
    WdfTimerStart(c->StatsTimer, WDF_REL_TIMEOUT_IN_MS(2000));
    Join_SelfTest(dev, c);
    return STATUS_SUCCESS;
}

static VOID EvtJoinWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    LONG job = c->JoinJob;

    if (job == JOB_JOIN) {
        NTSTATUS st = c->HwReady ? Join_Run(dev, c) : STATUS_DEVICE_NOT_READY;
        c->JoinRuns++;
        RegLog(dev, L"Log_Join_Runs", c->JoinRuns);
        RegLog(dev, L"Log_Join_Status", (ULONG)st);
        if (!NT_SUCCESS(st)) {
            InterlockedExchange(&c->JoinState, JOIN_IDLE);
            Join_HwDown(c);
        }
        if (!c->Stopping)
            WifiCx_OnConnectResult(dev, st, c->JoinStatus, c->JoinAid, c->JoinBssid,
                                   c->AssocReq, c->AssocReqLen, c->AssocResp, c->AssocRespLen);
    } else if (job == JOB_LEAVE) {
        UCHAR f[26];
        if (c->JoinState == JOIN_UP && c->HwReady) {
            Mgmt_Header(c, f, 0xC0);          /* deauthentication, reason 3 = leaving */
            f[24] = 3; f[25] = 0;
            (VOID)Tx_Raw(c, f, sizeof(f), TRUE, FALSE);
        }
        InterlockedExchange(&c->JoinState, JOIN_IDLE);
        if (c->HwReady) Join_HwDown(c);
        if (!c->Stopping) WifiCx_OnDisconnectDone(dev);
        if (c->HwReady && !c->Stopping) ScanStart(c);          /* refresh the BSS list */
    }
}

static VOID LogDataStats(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    ULONG st[15] = {0};
    static const PCWSTR nm[15] = { L"Log_Dp_TxQCreated", L"Log_Dp_RxQCreated", L"Log_Dp_TxAdv", L"Log_Dp_TxPkt",
        L"Log_Dp_TxSubmitOk", L"Log_Dp_TxSubmitFail", L"Log_Dp_TxLastSt", L"Log_Dp_RxAdv", L"Log_Dp_RxArm",
        L"Log_Dp_RxInd", L"Log_Dp_RxRingDrop", L"Log_Dp_RxNotify", L"Log_Dp_TxL2Type", L"Log_Dp_TxL2HdrLen", L"Log_Dp_TxL3Type" };
    ULONG i;
    WifiCx_GetDataStats(dev, st, 15);
    for (i = 0; i < 15; i++) RegLog(dev, nm[i], st[i]);
    RegLog(dev, L"Log_Data_TxOk", c->DataTxOk);
    RegLog(dev, L"Log_Data_TxFail", c->DataTxFail);
    RegLog(dev, L"Log_Data_TxDrop", c->DataTxDrop);
    RegLog(dev, L"Log_Data_Rx", c->DataRx);
    RegLog(dev, L"Log_Data_RxDrop", c->DataRxDrop);
    RegLog(dev, L"Log_Data_RxFrames", c->RxCallbacks);
    RegLog(dev, L"Log_Rx_Type2", c->RxType2);
    RegLog(dev, L"Log_Rx_FltNotFromDs", c->RxFlt[0]);
    RegLog(dev, L"Log_Rx_FltBssid", c->RxFlt[1]);
    RegLog(dev, L"Log_Rx_FltNoData", c->RxFlt[2]);
    RegLog(dev, L"Log_Rx_FltDa", c->RxFlt[3]);
    RegLog(dev, L"Log_Rx_FltShort", c->RxFlt[4]);
    RegLog(dev, L"Log_Rx_FltLlc", c->RxFlt[5]);
    RegLog(dev, L"Log_Rx_FirstFc", c->RxFirstFc);
    RegLog(dev, L"Log_Rx_FirstV0", c->RxFirstV0);
    RegLog(dev, L"Log_Rx_DataAny", c->RxDataAny);
    RegLog(dev, L"Log_Dbg_TxAllSinceUp", c->TxAll);
    { ULONG q; WCHAR cn[24]; for (q = 0; q < 5; q++) { RtlStringCbPrintfW(cn, sizeof(cn), L"Log_Dbg_TxCls%u", q); RegLog(dev, cn, c->TxCls[q]); } }
    RegLog(dev, L"Log_Dbg_TxNative", c->TxNative);
    RegLog(dev, L"Log_Sec_TxEnc", c->TxEnc);
    RegLog(dev, L"Log_Sec_RxProt", c->RxProt);
    RegLog(dev, L"Log_Sec_RxProtDrop", c->RxProtDrop);
    RegLog(dev, L"Log_Sec_RxProtV0", c->RxProtV0);
    RegLog(dev, L"Log_Dbg_RxNative", c->RxNative);
    RegLog(dev, L"Log_Dbg_TxFirstMs", c->TxFirstMs);
    RegLog(dev, L"Log_Dbg_TxLastMs", c->TxLastMs);
    {
        static const WCHAR hx[] = L"0123456789abcdef";
        WCHAR buf[260], nm[24];
        ULONG k, j, n;
        for (k = 0; k < 22; k++) {
            const UCHAR *src; ULONG len, nb, ms = 0;
            if (k < 10) { if (!(c->DbgTxN & (1u << k))) continue; src = c->DbgTx[k]; len = c->DbgTxLen[k]; ms = c->DbgTxMs[k]; RtlStringCbPrintfW(nm, sizeof(nm), L"Log_DbgTxC%u_%u", k / 2, k % 2); }
            else { if (k - 10 >= c->DbgRxN) continue; src = c->DbgRx[k-10]; len = c->DbgRxLen[k-10]; ms = c->DbgRxMs[k-10]; RtlStringCbPrintfW(nm, sizeof(nm), L"Log_DbgRx%02u", k - 10); }
            nb = len > 96 ? 96 : len;
            n = 0;
            RtlStringCbPrintfW(buf, sizeof(buf), L"ms=%u len=%u ", ms, len);
            n = (ULONG)wcslen(buf);
            for (j = 0; j < nb && n + 3 < 258; j++) { buf[n++] = hx[src[j] >> 4]; buf[n++] = hx[src[j] & 15]; }
            buf[n] = 0;
            RegLogStr(dev, nm, buf);
        }
    }
    for (i = 0; i < 48; i++) {
        WCHAR n1[] = L"Log_RxH_T0S00", n2[] = L"Log_RxU_T0S00";
        n1[9] = n2[9] = (WCHAR)(L'0' + i / 16);
        n1[11] = n2[11] = (WCHAR)(L'0' + (i % 16) / 10);
        n1[12] = n2[12] = (WCHAR)(L'0' + (i % 16) % 10);
        if (c->RxH[i]) RegLog(dev, n1, c->RxH[i]);
        if (c->RxU[i]) RegLog(dev, n2, c->RxU[i]);
    }
}

VOID EvtStatsTimer(WDFTIMER timer)
{
    PDEVICE_CONTEXT c = GetDeviceContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    WdfWorkItemEnqueue(c->StatsWork);
}

static VOID EvtStatsWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    if (c->Stopping) return;
    c->TxMode = (LONG)RegGetUlong(dev, L"Cfg_TxMode", 0);
    c->TxRate = (LONG)RegGetUlong(dev, L"Cfg_TxRate", TXD_RATE_DATA);
    RegLog(dev, L"Log_TxMode_Active", (ULONG)c->TxMode);
    RegLog(dev, L"Log_TxRate_Active", (ULONG)c->TxRate);
    LogDataStats(dev, c);
    if (c->JoinState == JOIN_UP && c->HwReady) WdfTimerStart(c->StatsTimer, WDF_REL_TIMEOUT_IN_MS(2000));
}

static VOID EvtLossWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    if (c->Stopping) return;
    if (c->HwReady) Join_HwDown(c);
    WifiCx_OnLinkLost(dev, c->LossReason);
}

/* ---- data TX queue (PASSIVE worker drains frames queued at DISPATCH) ---- */

static VOID EvtTxWork(WDFWORKITEM wi)
{
    WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(wi);
    PDEVICE_CONTEXT c = GetDeviceContext(dev);

    /* a work item may run concurrently with itself: only one drainer, and re-check after releasing */
    for (;;) {
        BOOLEAN more;
        if (InterlockedCompareExchange(&c->TxWorkRunning, 1, 0) != 0) return;
        for (;;) {
            ULONG idx, len;
            BOOLEAN have;
            WdfSpinLockAcquire(c->TxQLock);
            have = (c->TxQHead != c->TxQTail);
            idx = c->TxQTail % TXQ_N;
            len = c->TxQLen[idx];
            WdfSpinLockRelease(c->TxQLock);
            if (!have) break;
            if (c->JoinState == JOIN_UP && c->HwReady && !c->Stopping)
                (VOID)Tx_Raw(c, c->TxQ[idx], len, FALSE, (BOOLEAN)(c->TxQ[idx][16] & 1));
            WdfSpinLockAcquire(c->TxQLock);
            c->TxQTail++;
            WdfSpinLockRelease(c->TxQLock);
        }
        InterlockedExchange(&c->TxWorkRunning, 0);
        WdfSpinLockAcquire(c->TxQLock);
        more = (c->TxQHead != c->TxQTail);
        WdfSpinLockRelease(c->TxQLock);
        if (!more) break;
    }
}

/* ---- RX: management responses + data -> Ethernet ---- */

static VOID Rx_Frame(PDEVICE_CONTEXT c, const UCHAR *f, ULONG len, ULONG v0)
{
    UCHAR type, sub, fl;
    LONG state = c->JoinState;
    if (len < 10) return;
    type = (UCHAR)((f[0] >> 2) & 3);
    sub  = (UCHAR)(f[0] >> 4);
    fl   = f[1];
    if (type < 3) {
        c->RxH[type * 16 + sub]++;
        if (len >= 10 && RtlEqualMemory(f + 4, c->Mac, 6)) c->RxU[type * 16 + sub]++;
        if (type == 2 && !(sub & 4)) {
            c->RxDataAny++;
            if (c->DbgRxN < 12) {
                ULONG k = c->DbgRxN++, n = len > 96 ? 96 : len;
                RtlCopyMemory(c->DbgRx[k], f, n);
                c->DbgRxLen[k] = (USHORT)len;
                c->DbgRxMs[k] = (ULONG)(KeQueryInterruptTime() / 10000 - c->UpMs);
            }
        }
    }
    if (len < 24) return;

    if (type == 0) {
        if (!RtlEqualMemory(f + 10, c->JoinBssid, 6)) return;
        if (sub == 11 && state == JOIN_AUTH && len >= 30) {
            if (Le16(f + 26) != 2) return;
            c->JoinStatus = Le16(f + 28);
            KeSetEvent(&c->JoinEvent, 0, FALSE);
        } else if (sub == 1 && state == JOIN_ASSOC && len >= 30) {
            ULONG bl = len - 24;
            if (bl > sizeof(c->AssocResp)) bl = sizeof(c->AssocResp);
            c->JoinCap = Le16(f + 24);
            c->JoinStatus = Le16(f + 26);
            c->JoinAid = (USHORT)(Le16(f + 28) & 0x3FFF);
            RtlCopyMemory(c->AssocResp, f + 24, bl);
            c->AssocRespLen = bl;
            KeSetEvent(&c->JoinEvent, 0, FALSE);
        } else if ((sub == 12 || sub == 10) && state == JOIN_UP && len >= 26) {
            c->LossReason = Le16(f + 24);
            if (InterlockedCompareExchange(&c->JoinState, JOIN_IDLE, JOIN_UP) == JOIN_UP)
                WdfWorkItemEnqueue(c->LossWork);
        }
        return;
    }

    if (type == 2 && state == JOIN_UP) {
        ULONG hdr = 24, end = len;
        const UCHAR *llc;
        c->RxType2++;
        if (c->RxType2 == 1) { c->RxFirstFc = f[0] | (f[1] << 8); c->RxFirstV0 = v0; }
        if ((fl & 3) != 2) { c->RxFlt[0]++; return; }                      /* only From-DS */
        if (!RtlEqualMemory(f + 10, c->JoinBssid, 6)) { c->RxFlt[1]++; return; }
        if (sub & 0x04) { c->RxFlt[2]++; return; }                         /* null / no-data subtypes */
        if (!(f[4] & 1) && !RtlEqualMemory(f + 4, c->Mac, 6)) { c->RxFlt[3]++; return; }
        if (c->RxNativeMode) {                              /* WiFiCx wants native 802.11 frames (no FCS) */
            if (fl & 0x40) {                                /* CCMP: hw decrypted, keeps CCMP hdr + MIC */
                ULONG sec = (v0 >> 20) & 7, swdec = (v0 >> 27) & 1, hl = (sub & 8) ? 26 : 24, body;
                UCHAR tmp[1600];
                if ((fl & 0x80) && (sub & 8)) hl += 4;
                c->RxProt++;
                if (c->RxProt == 1) c->RxProtV0 = v0;
                if (swdec || sec == 0 || len < hl + 8 + 8 + 4 + 8 || len > sizeof(tmp) + 20) { c->DataRxDrop++; c->RxProtDrop++; return; }
                body = len - 4 - 8 - hl - 8;                /* minus FCS, MIC, header, CCMP hdr */
                RtlCopyMemory(tmp, f, hl);
                tmp[1] &= (UCHAR)~0x40;
                RtlCopyMemory(tmp + hl, f + hl + 8, body);
                c->DataRx++; c->RxNative++;
                WifiCx_OnRxFrame(c->Self, tmp, hl + body);
                return;
            }
            if (len < 28) return;
            c->DataRx++;
            c->RxNative++;
            WifiCx_OnRxFrame(c->Self, f, len - 4);
            return;
        }
        if (sub & 0x08) hdr += 2;                           /* QoS control */
        if ((fl & 0x80) && (sub & 0x08)) hdr += 4;          /* HT control */
        if (fl & 0x40) {                                    /* protected: hw decrypted keeps CCMP hdr + MIC */
            ULONG sec = (v0 >> 20) & 7, swdec = (v0 >> 27) & 1;
            if (swdec || sec == 0) { c->DataRxDrop++; return; }
            hdr += 8;
            if (end < hdr + 8) return;
            end -= 8;
        }
        if (end < hdr + 8) { c->RxFlt[4]++; return; }
        llc = f + hdr;
        if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 || llc[3] != 0 || llc[4] != 0) { c->RxFlt[5]++; return; }
        c->DataRx++;
        WifiCx_OnRxData(c->Self, f + 4, f + 16, llc + 6, f + hdr + 8, end - hdr - 8);
    }
}

static VOID Rx_Dispatch(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len)
{
    ULONG off = 0;
    while (off + RX_DESC_LEN <= len) {
        ULONG v0 = *(const ULONG *)(buf + off);
        ULONG v3 = *(const ULONG *)(buf + off + 12);
        ULONG pktLen = v0 & 0x3FFF;
        ULONG drvInfo = ((v0 >> 16) & 0xF) * 8;
        ULONG shift = (v0 >> 24) & 3;
        ULONG hdr = RX_DESC_LEN + drvInfo + shift;
        if (pktLen == 0 || off + hdr + pktLen > len) break;
        if (!(v0 & 0xC000) && ((v3 >> 14) & 3) == 0)        /* no CRC/ICV error, not a C2H report */
            Rx_Frame(c, buf + off + hdr, pktLen, v0);
        off += (hdr + pktLen + 127) & ~127u;
    }
}

/* ---- exported to wifi.cpp ---- */

NTSTATUS Rtl_WifiConnect(WDFDEVICE dev, const UCHAR *bssid, const UCHAR *ssid, ULONG ssidLen, UCHAR ch,
                         const UCHAR *extIe, ULONG extIeLen)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    if (!c->HwReady) return STATUS_DEVICE_NOT_READY;
    if (ssidLen > 32 || ch < 1 || ch > 14 || extIeLen > sizeof(c->JoinExtIe)) return STATUS_INVALID_PARAMETER;
    if (c->JoinState != JOIN_IDLE) return STATUS_DEVICE_BUSY;
    RtlCopyMemory(c->JoinBssid, bssid, 6);
    RtlZeroMemory(c->JoinSsid, sizeof(c->JoinSsid));
    RtlCopyMemory(c->JoinSsid, ssid, ssidLen);
    c->JoinSsidLen = (UCHAR)ssidLen;
    c->JoinCh = ch;
    c->JoinExtIeLen = extIeLen;
    if (extIeLen) RtlCopyMemory(c->JoinExtIe, extIe, extIeLen);
    c->AssocReqLen = c->AssocRespLen = 0;
    c->JoinAid = 0;
    WdfSpinLockAcquire(c->TxQLock);
    c->TxQTail = c->TxQHead;                       /* drop anything stale */
    WdfSpinLockRelease(c->TxQLock);
    c->JoinJob = JOB_JOIN;
    WdfWorkItemEnqueue(c->JoinWork);
    return STATUS_SUCCESS;
}

VOID Rtl_WifiDisconnect(WDFDEVICE dev)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    c->JoinJob = JOB_LEAVE;
    WdfWorkItemEnqueue(c->JoinWork);
}

NTSTATUS Rtl_TxEthernet(WDFDEVICE dev, const UCHAR *eth, ULONG len)
{
    PDEVICE_CONTEXT c = GetDeviceContext(dev);
    ULONG idx, fl;
    UCHAR *f;
    if (c->JoinState != JOIN_UP) return STATUS_DEVICE_NOT_READY;
    if (len < 14 || len > 1514) return STATUS_INVALID_PARAMETER;

    WdfSpinLockAcquire(c->TxQLock);
    if (c->TxQHead - c->TxQTail >= TXQ_N) {
        c->DataTxDrop++;
        WdfSpinLockRelease(c->TxQLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    idx = c->TxQHead % TXQ_N;
    f = c->TxQ[idx];
    if (len >= 32 && len <= TXQ_BUF && (eth[0] & 0x0F) == 0x08 && (eth[1] & 3) == 1 &&
        RtlEqualMemory(eth + 4, c->JoinBssid, 6)) {
        /* WiFiCx native 802.11 data frame (header + LLC already built by Windows): pass through */
        RtlCopyMemory(f, eth, len);
        f[2] = f[3] = 0;                                       /* duration/ID: let the hardware fill it */
        RtlCopyMemory(f + 10, c->Mac, 6);
        fl = len;
        c->TxNative++;
    } else {
    RtlZeroMemory(f, 26);
    {
        ULONG hl = 24;
        if (c->TxMode == 2) { f[0] = 0x88; hl = 26; }          /* QoS data, TID 0 */
        else f[0] = 0x08;
        f[1] = 0x01;                                           /* To-DS */
        RtlCopyMemory(f + 4, c->JoinBssid, 6);                 /* A1 = BSSID */
        RtlCopyMemory(f + 10, c->Mac, 6);                      /* A2 = SA   */
        RtlCopyMemory(f + 16, eth, 6);                         /* A3 = DA   */
        f[hl] = 0xAA; f[hl + 1] = 0xAA; f[hl + 2] = 0x03; f[hl + 3] = 0; f[hl + 4] = 0; f[hl + 5] = 0;
        f[hl + 6] = eth[12]; f[hl + 7] = eth[13];              /* ethertype */
        RtlCopyMemory(f + hl + 8, eth + 14, len - 14);
        fl = hl + 8 + len - 14;
    }
    }
    c->TxAll++;
    {
        ULONG cls = 4, ms = (ULONG)(KeQueryInterruptTime() / 10000 - c->UpMs);
        if (fl > 32) {
            if (f[24 + 6] == 0x08 && f[24 + 7] == 0x06) cls = 0;
            else if (f[24 + 6] == 0x08 && f[24 + 7] == 0x00) cls = (fl > 58 && f[32 + 9] == 17 && f[32 + 22] == 0 && f[32 + 23] == 67) ? 1 : 2;
            else if (f[24 + 6] == 0x86 && f[24 + 7] == 0xDD) cls = 3;
        }
        if (c->TxAll == 1) c->TxFirstMs = ms;
        c->TxLastMs = ms;
        if (c->TxCls[cls]++ < 2) {
            ULONG k = cls * 2 + (c->TxCls[cls] - 1), n = fl > 96 ? 96 : fl;
            RtlCopyMemory(c->DbgTx[k], f, n);
            c->DbgTxLen[k] = (USHORT)fl;
            c->DbgTxMs[k] = ms;
            c->DbgTxN |= 1u << k;
        }
    }
    /* TxQ[idx][16] is A3[0]: used by the worker for the multicast bit */
    c->TxQLen[idx] = (USHORT)fl;
    c->TxQHead++;
    WdfSpinLockRelease(c->TxQLock);
    WdfWorkItemEnqueue(c->TxWork);
    return STATUS_SUCCESS;
}

/* ---- driver ------------------------------------------------------------ */

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG tcfg;
    WDF_WORKITEM_CONFIG wcfg;
    WDFDEVICE dev;
    PDEVICE_CONTEXT c;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(drv);

    st = WifiCx_DeviceInitConfig(init);          /* NetDeviceInitConfig + WifiDeviceInitConfig */
    if (!NT_SUCCESS(st)) return st;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnp.EvtDeviceD0Entry = EvtDeviceD0Entry;
    pnp.EvtDeviceD0Exit  = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&init, &attr, &dev);
    if (!NT_SUCCESS(st)) return st;
    c = GetDeviceContext(dev);
    c->WdfTriageInfoPtr = WdfGetTriageInfo();

    st = WifiCx_DeviceInitialize(dev);           /* WifiDeviceInitialize */
    if (!NT_SUCCESS(st)) return st;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfSpinLockCreate(&attr, &c->BssLock);
    if (!NT_SUCCESS(st)) return st;

    WDF_TIMER_CONFIG_INIT(&tcfg, EvtScanTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfTimerCreate(&tcfg, &attr, &c->ScanTimer);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtScanWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWorkItemCreate(&wcfg, &attr, &c->ScanWork);
    if (!NT_SUCCESS(st)) return st;

    /* phase 5d */
    c->Self = dev;
    KeInitializeEvent(&c->JoinEvent, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWaitLockCreate(&attr, &c->TxLock);
    if (!NT_SUCCESS(st)) return st;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfSpinLockCreate(&attr, &c->TxQLock);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtStatsWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWorkItemCreate(&wcfg, &attr, &c->StatsWork);
    if (!NT_SUCCESS(st)) return st;
    WDF_TIMER_CONFIG_INIT(&tcfg, EvtStatsTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfTimerCreate(&tcfg, &attr, &c->StatsTimer);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtJoinWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWorkItemCreate(&wcfg, &attr, &c->JoinWork);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtLossWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWorkItemCreate(&wcfg, &attr, &c->LossWork);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtKeyWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    st = WdfWorkItemCreate(&wcfg, &attr, &c->KeyWork);
    if (!NT_SUCCESS(st)) return st;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, EvtTxWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = dev;
    return WdfWorkItemCreate(&wcfg, &attr, &c->TxWork);
}

/* PrepareHardware: USB plumbing + EFUSE only. Fast, no radio bring-up. */
NTSTATUS EvtDevicePrepareHardware(WDFDEVICE dev, WDFCMRESLIST res, WDFCMRESLIST resTr)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(dev);
    WDF_USB_DEVICE_CREATE_CONFIG ucfg;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS sel;
    WDF_USB_CONTINUOUS_READER_CONFIG rcfg;
    NTSTATUS st;
    UCHAR i, npipes;
    ULONG sysCfg = 0, rawLen = 0;
    USHORT r9346 = 0;
    WCHAR nm[32];

    UNREFERENCED_PARAMETER(res);
    UNREFERENCED_PARAMETER(resTr);

    RegLog(dev, L"Log_Stage", 1);   /* entered PrepareHardware */

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&ucfg, USBD_CLIENT_CONTRACT_VERSION_602);
    st = WdfUsbTargetDeviceCreateWithParameters(dev, &ucfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->UsbDevice);
    RegLog(dev, L"Log_UsbDeviceCreate", (ULONG)st);
    if (!NT_SUCCESS(st)) { LOG("UsbDeviceCreate failed 0x%08x", st); return st; }
    RegLog(dev, L"Log_Stage", 2);   /* USB device object ok */

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&sel);
    st = WdfUsbTargetDeviceSelectConfig(ctx->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &sel);
    RegLog(dev, L"Log_SelectConfig", (ULONG)st);
    if (!NT_SUCCESS(st)) { LOG("SelectConfig failed 0x%08x", st); return st; }
    RegLog(dev, L"Log_Stage", 3);   /* configuration selected */

    ctx->UsbInterface = sel.Types.SingleInterface.ConfiguredUsbInterface;
    npipes = WdfUsbInterfaceGetNumConfiguredPipes(ctx->UsbInterface);
    RegLog(dev, L"Log_NumPipes", npipes);

    ctx->BulkIn = NULL; ctx->TxPipe = NULL; ctx->TxPipeData = NULL; ctx->BulkOutCount = 0;
    for (i = 0; i < npipes; i++) {
        WDF_USB_PIPE_INFORMATION pi;
        WDFUSBPIPE pipe;
        WDF_USB_PIPE_INFORMATION_INIT(&pi);
        pipe = WdfUsbInterfaceGetConfiguredPipe(ctx->UsbInterface, i, &pi);
        LOG("  pipe %u: addr=0x%02x type=%d maxpkt=%u", i, pi.EndpointAddress, (int)pi.PipeType, pi.MaximumPacketSize);

        /* Log_PipeN = addr | (type << 8) | (maxpkt << 16)   type: 3=bulk */
        if (NT_SUCCESS(RtlStringCchPrintfW(nm, 32, L"Log_Pipe%u", (ULONG)i)))
            RegLog(dev, nm, (ULONG)pi.EndpointAddress | ((ULONG)pi.PipeType << 8) | ((ULONG)pi.MaximumPacketSize << 16));

        if (pi.PipeType != WdfUsbPipeTypeBulk) continue;
        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            ctx->BulkIn = pipe;
        } else if (WdfUsbTargetPipeIsOutEndpoint(pipe) && ctx->BulkOutCount < MAX_BULK_OUT) {
            ctx->BulkOut[ctx->BulkOutCount++] = pipe;
            if (pi.EndpointAddress == TX_ENDPOINT) ctx->TxPipe = pipe;
            else if (ctx->TxPipeData == NULL) ctx->TxPipeData = pipe;
        }
    }
    RegLog(dev, L"Log_BulkIn", ctx->BulkIn ? 1 : 0);
    RegLog(dev, L"Log_BulkOutCount", ctx->BulkOutCount);
    RegLog(dev, L"Log_TxPipe", ctx->TxPipe ? 1 : 0);
    RegLog(dev, L"Log_TxPipeData", ctx->TxPipeData ? 1 : 0);
    if (!ctx->BulkIn) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RegLog(dev, L"Log_Stage", 4);   /* pipes enumerated */

    /* RX: WDF owns the read loop; the framework starts it after D0Entry returns */
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&rcfg, EvtUsbRxComplete, (WDFCONTEXT)dev, RX_BUF_SIZE);
    rcfg.NumPendingReads = 2;
    rcfg.EvtUsbTargetPipeReadersFailed = EvtUsbReadersFailed;
    st = WdfUsbTargetPipeConfigContinuousReader(ctx->BulkIn, &rcfg);
    RegLog(dev, L"Log_Reader_Status", (ULONG)st);
    if (!NT_SUCCESS(st)) return st;

    st = Rtl_Read32(ctx, REG_SYS_CFG, &sysCfg);
    RegLog(dev, L"Log_SysCfg_Status", (ULONG)st);
    RegLog(dev, L"Log_SysCfg_Value", sysCfg);
    st = Rtl_Read16(ctx, REG_9346CR, &r9346);
    RegLog(dev, L"Log_Reg0A_Status", (ULONG)st);
    RegLog(dev, L"Log_Reg0A_Value", r9346);
    RegLog(dev, L"Log_Stage", 5);   /* register access works */

    /* EFUSE -> MAC, TX power, crystal. Log_MacA = b0|b1<<8|b2<<16|b3<<24, Log_MacB = b4|b5<<8 */
    st = Rtl_ReadEfuse(ctx, &rawLen);
    RegLog(dev, L"Log_Efuse_Status", (ULONG)st);
    RegLog(dev, L"Log_Efuse_RawLen", rawLen);
    if (NT_SUCCESS(st)) {
        const UCHAR *m = ctx->EfuseMap + EFUSE_MAC_ADDR_88EU;
        RtlCopyMemory(ctx->Mac, m, 6);
        RegLog(dev, L"Log_MacA", (ULONG)m[0] | ((ULONG)m[1] << 8) | ((ULONG)m[2] << 16) | ((ULONG)m[3] << 24));
        RegLog(dev, L"Log_MacB", (ULONG)m[4] | ((ULONG)m[5] << 8));
        RegLog(dev, L"Log_Stage", 6);   /* efuse parsed */
    }
    /* EFUSE failure is not fatal for the debug build: keep the device so logs can be read */

    /* phase 5b: tell WiFiCx what this adapter can do (Log_Wifi_Cap* hold per-step status) */
    st = WifiCx_SetCapabilities(dev, ctx->Mac);
    RegLog(dev, L"Log_Wifi_Caps", (ULONG)st);
    RegLog(dev, L"Log_Stage", 7);   /* wifi capabilities set */
    return NT_SUCCESS(st) ? STATUS_SUCCESS : st;
}

/* D0Entry: full radio bring-up. Runs on first start and after every resume.
 * Debug build: failures are logged and the device stays loaded (HwReady stays 0). */
NTSTATUS EvtDeviceD0Entry(WDFDEVICE dev, WDF_POWER_DEVICE_STATE prev)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(dev);
    NTSTATUS st;
    ULONG reg80 = 0, polls = 0, initDone = 0, init2Done = 0;
    ULONG vMismatch = 0, vFirstReg = 0, vFirstGot = 0, xtalCap = 0, xtalReg = 0;
    UNREFERENCED_PARAMETER(prev);

    ctx->D0Count++;
    RegLog(dev, L"Log_D0Entry_Count", ctx->D0Count);
    RegLog(dev, L"Log_D0Entry_Prev", (ULONG)prev);
    InterlockedExchange(&ctx->HwReady, 0);
    InterlockedExchange(&ctx->Stopping, 0);
    InterlockedExchange(&ctx->JoinState, 0);

    st = Rtl_PowerOn(ctx);
    RegLog(dev, L"Log_PowerOn_Status", (ULONG)st);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;
    RegLog(dev, L"Log_Stage", 7);   /* power on done */

    st = Rtl_DownloadFirmware(ctx, &reg80, &polls);
    RegLog(dev, L"Log_Fw_Status", (ULONG)st);
    RegLog(dev, L"Log_Fw_Reg80", reg80);
    RegLog(dev, L"Log_Fw_Polls", polls);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;
    RegLog(dev, L"Log_Stage", 8);   /* firmware running (WINTINI_RDY) */

    st = Rtl_ReplayInit(ctx, &initDone);
    RegLog(dev, L"Log_Init_Status", (ULONG)st);
    RegLog(dev, L"Log_Init_Done", initDone);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;
    RegLog(dev, L"Log_Stage", 9);   /* tables written */

    st = Rtl_VerifyInit(ctx, dev, &vMismatch, &vFirstReg, &vFirstGot);
    RegLog(dev, L"Log_Verify_Status", (ULONG)st);
    RegLog(dev, L"Log_Verify_Count", RTL_VERIFY_COUNT);
    RegLog(dev, L"Log_Verify_Mismatch", vMismatch);
    RegLog(dev, L"Log_Verify_FirstReg", vFirstReg);
    RegLog(dev, L"Log_Verify_FirstGot", vFirstGot);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;
    RegLog(dev, L"Log_Stage", 10);  /* verify finished */

    st = Rtl_ReplayInit2(ctx, &init2Done);
    RegLog(dev, L"Log_Init2_Status", (ULONG)st);
    RegLog(dev, L"Log_Init2_Done", init2Done);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;

    st = Rtl_ApplyCrystal(ctx, &xtalCap, &xtalReg);
    RegLog(dev, L"Log_Xtal_Status", (ULONG)st);
    RegLog(dev, L"Log_Xtal_Cap", xtalCap);
    RegLog(dev, L"Log_Xtal_Reg24", xtalReg);
    RegLog(dev, L"Log_Pwr_Mismatch", Rtl_TxPowerSelfCheck(ctx));
    RegLog(dev, L"Log_Stage", 11);  /* RF/cal replayed */

    InterlockedExchange(&ctx->HwReady, 1);

    /* Make sure the continuous reader is actually running (osrusbfx2 does the same):
     * start the bulk-IN I/O target explicitly and log its state. */
    {
        WDFIOTARGET tgt = WdfUsbTargetPipeGetIoTarget(ctx->BulkIn);
        RegLog(dev, L"Log_Rx_StateBefore", (ULONG)WdfIoTargetGetState(tgt));
        st = WdfIoTargetStart(tgt);
        RegLog(dev, L"Log_Rx_StartStatus", (ULONG)st);
        RegLog(dev, L"Log_Rx_StateAfter", (ULONG)WdfIoTargetGetState(tgt));
    }
    RegLog(dev, L"Log_Stage", 12);  /* hardware ready, scan starting */
    ScanStart(ctx);                 /* async: returns immediately */
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceD0Exit(WDFDEVICE dev, WDF_POWER_DEVICE_STATE target)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(dev);
    UNREFERENCED_PARAMETER(target);

    InterlockedExchange(&ctx->Stopping, 1);
    InterlockedExchange(&ctx->JoinState, 0);
    KeSetEvent(&ctx->JoinEvent, 0, FALSE);
    InterlockedExchange(&ctx->ScanCancel, 1);
    InterlockedExchange(&ctx->HwReady, 0);
    WdfTimerStop(ctx->StatsTimer, TRUE);
    WdfWorkItemFlush(ctx->StatsWork);
    WdfWorkItemFlush(ctx->JoinWork);
    WdfWorkItemFlush(ctx->LossWork);
    WdfWorkItemFlush(ctx->TxWork);
    WdfWorkItemFlush(ctx->KeyWork);
    /* stop+flush twice: a work item that was already running may re-arm the timer once */
    WdfTimerStop(ctx->ScanTimer, TRUE);
    WdfWorkItemFlush(ctx->ScanWork);
    WdfTimerStop(ctx->ScanTimer, TRUE);
    WdfWorkItemFlush(ctx->ScanWork);
    InterlockedExchange(&ctx->ScanRunning, 0);
    if (ctx->BulkIn) WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(ctx->BulkIn), WdfIoTargetCancelSentIo);
    RegLog(dev, L"Log_D0Exit_Count", ctx->D0Count);
    RegLog(dev, L"Log_D0Exit_Target", (ULONG)target);
    return STATUS_SUCCESS;
}
