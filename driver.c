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

#include "fwdata.h"
#include "inittab.h"
#include "inittab2.h"

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

typedef struct _DEVICE_CONTEXT {
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
    struct {
        UCHAR  Bssid[6];
        UCHAR  Ssid[33];
        UCHAR  Ch;
        UCHAR  RxCh;
        ULONG  Hits;
        ULONG  Resp;                        /* probe responses seen */
    } Bss[RTL_MAX_BSS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;

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

static VOID Scan_AddBss(PDEVICE_CONTEXT c, const UCHAR *bssid, const UCHAR *ssid, UCHAR ssidLen, UCHAR bssCh, UCHAR rxCh, BOOLEAN isResp)
{
    ULONG i;
    if (ssidLen > 32) ssidLen = 32;
    for (i = 0; i < c->BssCount; i++) {
        if (RtlCompareMemory(c->Bss[i].Bssid, bssid, 6) == 6) {
            c->Bss[i].Hits++;
            if (isResp) c->Bss[i].Resp++;
            /* hidden AP: beacon has empty/zeroed SSID, probe response carries the real one */
            if (c->Bss[i].Ssid[0] == 0 && ssidLen > 0 && ssid[0] != 0) {
                RtlCopyMemory(c->Bss[i].Ssid, ssid, ssidLen);
                c->Bss[i].Ssid[ssidLen] = 0;
            }
            return;
        }
    }
    if (c->BssCount >= RTL_MAX_BSS) return;
    RtlCopyMemory(c->Bss[i].Bssid, bssid, 6);
    RtlCopyMemory(c->Bss[i].Ssid, ssid, ssidLen);
    c->Bss[i].Ssid[ssidLen] = 0;
    c->Bss[i].Ch = bssCh;
    c->Bss[i].RxCh = rxCh;
    c->Bss[i].Hits = 1;
    c->Bss[i].Resp = isResp ? 1 : 0;
    c->BssCount++;
}

static VOID Scan_ProcessFrame(PDEVICE_CONTEXT c, const UCHAR *f, ULONG len, UCHAR rxCh)
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
    Scan_AddBss(c, f + 16, ssid, ssidLen, ch, rxCh, (BOOLEAN)(subtype == 5));
}

static VOID Scan_ParseBuffer(PDEVICE_CONTEXT c, const UCHAR *buf, ULONG len, UCHAR rxCh)
{
    ULONG off = 0;
    while (off + RX_DESC_LEN <= len) {
        ULONG v0 = *(const ULONG *)(buf + off);
        ULONG pktLen = v0 & 0x3FFF;
        ULONG drvInfo = ((v0 >> 16) & 0xF) * 8;
        ULONG shift = (v0 >> 24) & 3;
        ULONG hdr = RX_DESC_LEN + drvInfo + shift;
        ULONG adv;
        if (pktLen == 0 || off + hdr + pktLen > len) break;
        if (v0 & 0xC000) c->ScanBadCrc++;             /* CRC32 / ICV error */
        else Scan_ProcessFrame(c, buf + off + hdr, pktLen, rxCh);
        adv = (hdr + pktLen + 127) & ~127u;            /* entries are 128-byte aligned */
        off += adv;
    }
}

static NTSTATUS Rtl_Scan(PDEVICE_CONTEXT c, ULONG *readsOk, ULONG *readsTimeout)
{
    PUCHAR buf;
    UCHAR ch;
    NTSTATUS st = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;

    if (!c->BulkIn) return STATUS_DEVICE_NOT_READY;
    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, RX_BUF_SIZE, 'ur8R');
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(60));

    c->ScanFrames = c->ScanMgmt = c->ScanBadCrc = c->BssCount = 0;
    RtlZeroMemory(c->Bss, sizeof(c->Bss));
    *readsOk = *readsTimeout = 0;

    CHK(Rtl_Write32(c, 0x0608, 0x7000600E));           /* RCR: accept all mgmt/beacons (scan mode) */

    for (ch = 1; ch <= 13; ch++) {
        ULONGLONG until;
        st = Rtl_SetChannel(c, ch);
        if (!NT_SUCCESS(st)) break;
        until = KeQueryInterruptTime() + SCAN_DWELL_100NS;
        while (KeQueryInterruptTime() < until) {
            ULONG_PTR got = 0;
            NTSTATUS rs;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, buf, RX_BUF_SIZE);
            rs = WdfUsbTargetPipeReadSynchronously(c->BulkIn, WDF_NO_HANDLE, &opts, &md, (PULONG)&got);
            if (NT_SUCCESS(rs) && got > 0) { (*readsOk)++; Scan_ParseBuffer(c, buf, (ULONG)got, ch); }
            else (*readsTimeout)++;
        }
    }
    ExFreePoolWithTag(buf, 'ur8R');
    return st;
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

static NTSTATUS Rtl_ActiveScan(PDEVICE_CONTEXT c, ULONG *readsOk, ULONG *readsTimeout)
{
    PUCHAR buf;
    UCHAR ch;
    ULONG consecFail = 0;
    NTSTATUS st = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;

    if (!c->BulkIn || !c->TxPipe) return STATUS_DEVICE_NOT_READY;
    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, RX_BUF_SIZE, 'ur8R');
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(40));
    *readsOk = *readsTimeout = 0;

    for (ch = 1; ch <= 13; ch++) {
        ULONGLONG start, until;
        BOOLEAN second = FALSE;

        st = Rtl_TxChannelSetup(c, ch);
        if (NT_SUCCESS(st)) st = Rtl_SetChannel(c, ch);
        if (!NT_SUCCESS(st)) break;

        start = KeQueryInterruptTime();
        until = start + ACT_DWELL_100NS;
        if (NT_SUCCESS(Rtl_SendProbe(c, ch, NULL, 0))) consecFail = 0; else consecFail++;

        while (KeQueryInterruptTime() < until && consecFail < TX_MAX_CONSEC_FAIL) {
            ULONG_PTR got = 0;
            NTSTATUS rs;
            if (!second && KeQueryInterruptTime() >= start + ACT_PROBE2_100NS) {
                second = TRUE;
                if (NT_SUCCESS(Rtl_SendProbe(c, ch, NULL, 0))) consecFail = 0; else consecFail++;
            }
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, buf, RX_BUF_SIZE);
            rs = WdfUsbTargetPipeReadSynchronously(c->BulkIn, WDF_NO_HANDLE, &opts, &md, (PULONG)&got);
            if (NT_SUCCESS(rs) && got > 0) { (*readsOk)++; Scan_ParseBuffer(c, buf, (ULONG)got, ch); }
            else (*readsTimeout)++;
        }
        if (consecFail >= TX_MAX_CONSEC_FAIL) { st = c->TxFirstErr; break; }   /* TX path dead, stop early */
    }
    ExFreePoolWithTag(buf, 'ur8R');
    return st;
}

/* Directed probe test: for every BSS with a known SSID, hop to its channel, send a probe
 * carrying that SSID, listen 100 ms. Proves the variable-length TX path needed for connect. */
static NTSTATUS Rtl_DirectedProbeTest(PDEVICE_CONTEXT c)
{
    PUCHAR buf;
    ULONG i;
    NTSTATUS st = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opts;

    if (!c->BulkIn || !c->TxPipe) return STATUS_DEVICE_NOT_READY;
    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, RX_BUF_SIZE, 'ur8R');
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(40));

    for (i = 0; i < c->BssCount; i++) {
        UCHAR ch = c->Bss[i].Ch, n = 0;
        ULONGLONG until;
        if (c->Bss[i].Ssid[0] == 0) continue;                /* hidden: SSID unknown */
        if (ch < 1 || ch > 13) ch = c->Bss[i].RxCh;
        while (n < 32 && c->Bss[i].Ssid[n]) n++;
        st = Rtl_TxChannelSetup(c, ch);
        if (NT_SUCCESS(st)) st = Rtl_SetChannel(c, ch);
        if (!NT_SUCCESS(st)) break;
        if (NT_SUCCESS(Rtl_SendProbe(c, ch, c->Bss[i].Ssid, n))) c->DirSent++;
        until = KeQueryInterruptTime() + 1000000LL;          /* 100 ms */
        while (KeQueryInterruptTime() < until) {
            ULONG_PTR got = 0;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, buf, RX_BUF_SIZE);
            if (NT_SUCCESS(WdfUsbTargetPipeReadSynchronously(c->BulkIn, WDF_NO_HANDLE, &opts, &md, (PULONG)&got)) && got > 0)
                Scan_ParseBuffer(c, buf, (ULONG)got, ch);
        }
    }
    ExFreePoolWithTag(buf, 'ur8R');
    return st;
}

static VOID LogBssList(WDFDEVICE dev, PDEVICE_CONTEXT c)
{
    ULONG bi;
    for (bi = 0; bi < c->BssCount; bi++) {
        WCHAR name[24], val[112], ssw[34];
        ULONG k;
        for (k = 0; c->Bss[bi].Ssid[k] && k < 32; k++)
            ssw[k] = (c->Bss[bi].Ssid[k] >= 0x20 && c->Bss[bi].Ssid[k] < 0x7F) ? (WCHAR)c->Bss[bi].Ssid[k] : L'?';
        ssw[k] = 0;
        if (NT_SUCCESS(RtlStringCchPrintfW(name, 24, L"Scan_Bss%02u", bi)) &&
            NT_SUCCESS(RtlStringCchPrintfW(val, 112, L"%ws | %02x:%02x:%02x:%02x:%02x:%02x | ch%u (heard on %u) | hits %lu | resp %lu",
                ssw[0] ? ssw : L"<hidden>",
                c->Bss[bi].Bssid[0], c->Bss[bi].Bssid[1], c->Bss[bi].Bssid[2],
                c->Bss[bi].Bssid[3], c->Bss[bi].Bssid[4], c->Bss[bi].Bssid[5],
                (ULONG)c->Bss[bi].Ch, (ULONG)c->Bss[bi].RxCh, c->Bss[bi].Hits, c->Bss[bi].Resp)))
            RegLogStr(dev, name, val);
    }
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
    WDFDEVICE dev;
    UNREFERENCED_PARAMETER(drv);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    return WdfDeviceCreate(&init, &attr, &dev);
}

NTSTATUS EvtDevicePrepareHardware(WDFDEVICE dev, WDFCMRESLIST res, WDFCMRESLIST resTr)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(dev);
    WDF_USB_DEVICE_CREATE_CONFIG ucfg;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS sel;
    NTSTATUS st;
    UCHAR i, npipes;
    ULONG sysCfg = 0, rawLen = 0, reg80 = 0, polls = 0;
    ULONG initDone = 0, vMismatch = 0, vFirstReg = 0, vFirstGot = 0;
    ULONG init2Done = 0, rdOk = 0, rdTo = 0;
    ULONG actOk = 0, actTo = 0, snapFrames, snapMgmt, snapBad, snapBss;
    ULONG xtalCap = 0, xtalReg = 0, snapResp;
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
    LOG("configured pipes: %u", npipes);

    for (i = 0; i < npipes; i++) {
        WDF_USB_PIPE_INFORMATION pi;
        WDFUSBPIPE pipe;
        WDF_USB_PIPE_INFORMATION_INIT(&pi);
        pipe = WdfUsbInterfaceGetConfiguredPipe(ctx->UsbInterface, i, &pi);
        LOG("  pipe %u: addr=0x%02x type=%d maxpkt=%u",
            i, pi.EndpointAddress, (int)pi.PipeType, pi.MaximumPacketSize);

        /* Log_PipeN = addr | (type << 8) | (maxpkt << 16)   type: 3=bulk */
        if (NT_SUCCESS(RtlStringCchPrintfW(nm, 32, L"Log_Pipe%u", (ULONG)i)))
            RegLog(dev, nm, (ULONG)pi.EndpointAddress | ((ULONG)pi.PipeType << 8) | ((ULONG)pi.MaximumPacketSize << 16));

        if (pi.PipeType != WdfUsbPipeTypeBulk) continue;
        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            ctx->BulkIn = pipe;
        } else if (WdfUsbTargetPipeIsOutEndpoint(pipe) && ctx->BulkOutCount < MAX_BULK_OUT) {
            ctx->BulkOut[ctx->BulkOutCount++] = pipe;
            if (pi.EndpointAddress == TX_ENDPOINT) ctx->TxPipe = pipe;
        }
    }
    RegLog(dev, L"Log_BulkIn", ctx->BulkIn ? 1 : 0);
    RegLog(dev, L"Log_BulkOutCount", ctx->BulkOutCount);
    RegLog(dev, L"Log_TxPipe", ctx->TxPipe ? 1 : 0);
    LOG("bulk in=%p, bulk out count=%lu (expect 1 in, 2 out)", ctx->BulkIn, ctx->BulkOutCount);
    RegLog(dev, L"Log_Stage", 4);   /* pipes enumerated */

    /* ---- Phase 3a: register reads. Expect SYS_CFG=0x24403735 (608188213) */
    st = Rtl_Read32(ctx, REG_SYS_CFG, &sysCfg);
    RegLog(dev, L"Log_SysCfg_Status", (ULONG)st);
    RegLog(dev, L"Log_SysCfg_Value", sysCfg);
    LOG("read SYS_CFG(0x00F0) st=0x%08x val=0x%08x", st, sysCfg);

    st = Rtl_Read16(ctx, REG_9346CR, &r9346);
    RegLog(dev, L"Log_Reg0A_Status", (ULONG)st);
    RegLog(dev, L"Log_Reg0A_Value", r9346);
    RegLog(dev, L"Log_Stage", 5);   /* register access works */

    /* ---- Phase 3b-1: EFUSE -> MAC address. Expect 10:27:f5:99:50:56 for this adapter.
     * Log_MacA = b0 | b1<<8 | b2<<16 | b3<<24 (0x99f52710), Log_MacB = b4 | b5<<8 (0x5650) */
    st = Rtl_ReadEfuse(ctx, &rawLen);
    RegLog(dev, L"Log_Efuse_Status", (ULONG)st);
    RegLog(dev, L"Log_Efuse_RawLen", rawLen);
    if (NT_SUCCESS(st)) {
        const UCHAR *m = ctx->EfuseMap + EFUSE_MAC_ADDR_88EU;
        RtlCopyMemory(ctx->Mac, m, 6);
        RegLog(dev, L"Log_MacA", (ULONG)m[0] | ((ULONG)m[1] << 8) | ((ULONG)m[2] << 16) | ((ULONG)m[3] << 24));
        RegLog(dev, L"Log_MacB", (ULONG)m[4] | ((ULONG)m[5] << 8));
        LOG("EFUSE ok, raw=%lu MAC=%02x:%02x:%02x:%02x:%02x:%02x", rawLen, m[0], m[1], m[2], m[3], m[4], m[5]);
        RegLog(dev, L"Log_Stage", 6);   /* efuse parsed */
    }

    /* ---- Phase 3b-2: power on + firmware download */
    if (NT_SUCCESS(st)) {
        st = Rtl_PowerOn(ctx);
        RegLog(dev, L"Log_PowerOn_Status", (ULONG)st);
        if (NT_SUCCESS(st)) {
            RegLog(dev, L"Log_Stage", 7);   /* power on done */
            st = Rtl_DownloadFirmware(ctx, &reg80, &polls);
            RegLog(dev, L"Log_Fw_Status", (ULONG)st);
            RegLog(dev, L"Log_Fw_Reg80", reg80);
            RegLog(dev, L"Log_Fw_Polls", polls);
            LOG("firmware st=0x%08x reg80=0x%08x polls=%lu", st, reg80, polls);
            if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 8);   /* firmware running (WINTINI_RDY) */
        }
    }

    /* ---- Phase 3c: MAC/BB/AGC/RF table replay (only if firmware is up) */
    if (NT_SUCCESS(st)) {
        st = Rtl_ReplayInit(ctx, &initDone);
        RegLog(dev, L"Log_Init_Status", (ULONG)st);
        RegLog(dev, L"Log_Init_Done", initDone);
        LOG("init replay st=0x%08x done=%lu/%u", st, initDone, (unsigned)RTL_INIT_COUNT);
        if (NT_SUCCESS(st)) {
            RegLog(dev, L"Log_Stage", 9);   /* tables written */
            st = Rtl_VerifyInit(ctx, dev, &vMismatch, &vFirstReg, &vFirstGot);
            RegLog(dev, L"Log_Verify_Status", (ULONG)st);
            RegLog(dev, L"Log_Verify_Count", RTL_VERIFY_COUNT);
            RegLog(dev, L"Log_Verify_Mismatch", vMismatch);
            RegLog(dev, L"Log_Verify_FirstReg", vFirstReg);
            RegLog(dev, L"Log_Verify_FirstGot", vFirstGot);
            if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 10);   /* verify finished */
        }
    }

    /* ---- Phase 3d: RF init + calibration + RX config (replay with delays) */
    if (NT_SUCCESS(st)) {
        st = Rtl_ReplayInit2(ctx, &init2Done);
        RegLog(dev, L"Log_Init2_Status", (ULONG)st);
        RegLog(dev, L"Log_Init2_Done", init2Done);
        LOG("init2 replay st=0x%08x done=%lu/%u", st, init2Done, (unsigned)RTL_INIT2_COUNT);
        if (NT_SUCCESS(st)) {
            NTSTATUS xs = Rtl_ApplyCrystal(ctx, &xtalCap, &xtalReg);     /* phase 4c: EFUSE crystal cap */
            RegLog(dev, L"Log_Xtal_Status", (ULONG)xs);
            RegLog(dev, L"Log_Xtal_Cap", xtalCap);
            RegLog(dev, L"Log_Xtal_Reg24", xtalReg);
            RegLog(dev, L"Log_Pwr_Mismatch", Rtl_TxPowerSelfCheck(ctx)); /* 0 = EFUSE derivation == capture */
            RegLog(dev, L"Log_Stage", 11);   /* RF/cal replayed */
        }
    }

    /* ---- Phase 4a: passive scan, 13 channels x 300 ms (~4 s) */
    if (NT_SUCCESS(st)) {
        st = Rtl_Scan(ctx, &rdOk, &rdTo);
        RegLog(dev, L"Log_Scan_Status", (ULONG)st);
        RegLog(dev, L"Log_Scan_ReadsOk", rdOk);
        RegLog(dev, L"Log_Scan_ReadsTimeout", rdTo);
        RegLog(dev, L"Log_Scan_Frames", ctx->ScanFrames);
        RegLog(dev, L"Log_Scan_Mgmt", ctx->ScanMgmt);
        RegLog(dev, L"Log_Scan_BadCrc", ctx->ScanBadCrc);
        RegLog(dev, L"Log_Scan_BssCount", ctx->BssCount);
        LOG("scan st=0x%08x reads ok=%lu timeout=%lu frames=%lu mgmt=%lu bss=%lu",
            st, rdOk, rdTo, ctx->ScanFrames, ctx->ScanMgmt, ctx->BssCount);
        if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 12);   /* scan finished */
    }

    /* ---- Phase 4b: active scan (probe requests on bulk OUT ep 0x02), 13 ch x 150 ms */
    if (NT_SUCCESS(st)) {
        snapFrames = ctx->ScanFrames; snapMgmt = ctx->ScanMgmt; snapBad = ctx->ScanBadCrc; snapBss = ctx->BssCount;
        LogBssList(dev, ctx);   /* passive result stays visible even if active pass dies */
        st = Rtl_ActiveScan(ctx, &actOk, &actTo);
        RegLog(dev, L"Log_Act_Status", (ULONG)st);
        RegLog(dev, L"Log_Tx_Ok", ctx->TxOk);
        RegLog(dev, L"Log_Tx_Fail", ctx->TxFail);
        RegLog(dev, L"Log_Tx_FirstErr", (ULONG)ctx->TxFirstErr);
        RegLog(dev, L"Log_Act_ReadsOk", actOk);
        RegLog(dev, L"Log_Act_ReadsTimeout", actTo);
        RegLog(dev, L"Log_Act_Frames", ctx->ScanFrames - snapFrames);
        RegLog(dev, L"Log_Act_Mgmt", ctx->ScanMgmt - snapMgmt);
        RegLog(dev, L"Log_Act_BadCrc", ctx->ScanBadCrc - snapBad);
        RegLog(dev, L"Log_Act_ProbeResp", ctx->ScanProbeResp);
        RegLog(dev, L"Log_Act_NewBss", ctx->BssCount - snapBss);
        RegLog(dev, L"Log_Act_BssCount", ctx->BssCount);
        snapResp = ctx->ScanProbeResp;
        if (NT_SUCCESS(st)) {
            NTSTATUS ds = Rtl_DirectedProbeTest(ctx);
            RegLog(dev, L"Log_Dir_Status", (ULONG)ds);
            RegLog(dev, L"Log_Dir_Sent", ctx->DirSent);
            RegLog(dev, L"Log_Dir_Resp", ctx->ScanProbeResp - snapResp);
        }
        LogBssList(dev, ctx);
        LOG("active scan st=0x%08x tx ok=%lu fail=%lu probe_resp=%lu bss=%lu",
            st, ctx->TxOk, ctx->TxFail, ctx->ScanProbeResp, ctx->BssCount);
        if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 13);   /* active scan finished */
    }

    return STATUS_SUCCESS;   /* load anyway so we can inspect logs */
}
