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
#define SCAN_DWELL_100NS    3000000LL   /* 300 ms per channel */

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE      BulkIn;
    WDFUSBPIPE      BulkOut[MAX_BULK_OUT];
    ULONG           BulkOutCount;
    UCHAR           EfuseMap[EFUSE_MAP_LEN];
    ULONG           ScanFrames;
    ULONG           ScanMgmt;
    ULONG           ScanBadCrc;
    ULONG           BssCount;
    struct {
        UCHAR  Bssid[6];
        UCHAR  Ssid[33];
        UCHAR  Ch;
        UCHAR  RxCh;
        ULONG  Hits;
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

static VOID Scan_AddBss(PDEVICE_CONTEXT c, const UCHAR *bssid, const UCHAR *ssid, UCHAR ssidLen, UCHAR bssCh, UCHAR rxCh)
{
    ULONG i;
    for (i = 0; i < c->BssCount; i++) {
        if (RtlCompareMemory(c->Bss[i].Bssid, bssid, 6) == 6) { c->Bss[i].Hits++; return; }
    }
    if (c->BssCount >= RTL_MAX_BSS) return;
    RtlCopyMemory(c->Bss[i].Bssid, bssid, 6);
    if (ssidLen > 32) ssidLen = 32;
    RtlCopyMemory(c->Bss[i].Ssid, ssid, ssidLen);
    c->Bss[i].Ssid[ssidLen] = 0;
    c->Bss[i].Ch = bssCh;
    c->Bss[i].RxCh = rxCh;
    c->Bss[i].Hits = 1;
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

    p = f + 36;                                        /* 24 hdr + 12 fixed fields */
    end = f + len;
    while (p + 2 <= end) {
        UCHAR id = p[0], l = p[1];
        if (p + 2 + l > end) break;
        if (id == 0) { ssid = p + 2; ssidLen = l; }
        else if (id == 3 && l >= 1) ch = p[2];
        p += 2 + l;
    }
    Scan_AddBss(c, f + 16, ssid, ssidLen, ch, rxCh);
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
    ULONG init2Done = 0, rdOk = 0, rdTo = 0, bi;
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
        }
    }
    RegLog(dev, L"Log_BulkIn", ctx->BulkIn ? 1 : 0);
    RegLog(dev, L"Log_BulkOutCount", ctx->BulkOutCount);
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
        if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 11);   /* RF/cal replayed */
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
        for (bi = 0; bi < ctx->BssCount; bi++) {
            WCHAR name[24], val[96], ssw[34];
            ULONG k;
            for (k = 0; ctx->Bss[bi].Ssid[k] && k < 32; k++)
                ssw[k] = (ctx->Bss[bi].Ssid[k] >= 0x20 && ctx->Bss[bi].Ssid[k] < 0x7F) ? (WCHAR)ctx->Bss[bi].Ssid[k] : L'?';
            ssw[k] = 0;
            if (NT_SUCCESS(RtlStringCchPrintfW(name, 24, L"Scan_Bss%02u", bi)) &&
                NT_SUCCESS(RtlStringCchPrintfW(val, 96, L"%ws | %02x:%02x:%02x:%02x:%02x:%02x | ch%u (heard on %u) | hits %lu",
                    ssw[0] ? ssw : L"<hidden>",
                    ctx->Bss[bi].Bssid[0], ctx->Bss[bi].Bssid[1], ctx->Bss[bi].Bssid[2],
                    ctx->Bss[bi].Bssid[3], ctx->Bss[bi].Bssid[4], ctx->Bss[bi].Bssid[5],
                    (ULONG)ctx->Bss[bi].Ch, (ULONG)ctx->Bss[bi].RxCh, ctx->Bss[bi].Hits)))
                RegLogStr(dev, name, val);
        }
        LOG("scan st=0x%08x reads ok=%lu timeout=%lu frames=%lu mgmt=%lu bss=%lu",
            st, rdOk, rdTo, ctx->ScanFrames, ctx->ScanMgmt, ctx->BssCount);
        if (NT_SUCCESS(st)) RegLog(dev, L"Log_Stage", 12);   /* scan finished */
    }

    return STATUS_SUCCESS;   /* load anyway so we can inspect logs */
}
