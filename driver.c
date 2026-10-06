/*
 * rtl8188eu - Phase 3 skeleton (KMDF USB, ARM64)
 *
 * Milestone: bind to USB\VID_2357&PID_010C, select config, enumerate bulk
 * pipes, issue Realtek vendor register reads (bRequest 0x05) and log them.
 * Output goes to DbgPrintEx (view with DebugView -> Capture Kernel, or WinDbg).
 *
 * Register protocol (verified from usbmon capture of rtl8xxxu):
 *   read : bmRequestType 0xC0, bRequest 0x05, wValue = reg, wIndex = 0, wLength = 1/2/4
 *   write: bmRequestType 0x40, bRequest 0x05, wValue = reg, wIndex = 0, wLength = 1/2/4
 *   little-endian data.
 */
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <ntstrsafe.h>

#define TAG "rtl8188eu: "
#define LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, TAG fmt "\n", __VA_ARGS__)

#define RTL_VENDOR_REQ      0x05
#define REG_SYS_CFG         0x00F0   /* 32-bit; capture showed 0x24403735 */
#define REG_9346CR          0x000A   /* 16-bit; capture showed 0x0020     */
#define MAX_BULK_OUT        3

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE      BulkIn;
    WDFUSBPIPE      BulkOut[MAX_BULK_OUT];
    ULONG           BulkOutCount;
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

NTSTATUS Rtl_Read8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  *v) { return RtlCtrl(c, TRUE,  r, v, 1); }
NTSTATUS Rtl_Read16(PDEVICE_CONTEXT c, USHORT r, USHORT *v) { return RtlCtrl(c, TRUE,  r, v, 2); }
NTSTATUS Rtl_Read32(PDEVICE_CONTEXT c, USHORT r, ULONG  *v) { return RtlCtrl(c, TRUE,  r, v, 4); }
NTSTATUS Rtl_Write8 (PDEVICE_CONTEXT c, USHORT r, UCHAR  v) { return RtlCtrl(c, FALSE, r, &v, 1); }
NTSTATUS Rtl_Write16(PDEVICE_CONTEXT c, USHORT r, USHORT v) { return RtlCtrl(c, FALSE, r, &v, 2); }
NTSTATUS Rtl_Write32(PDEVICE_CONTEXT c, USHORT r, ULONG  v) { return RtlCtrl(c, FALSE, r, &v, 4); }

/* ---- registry log: HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_2357&PID_010C\<serial>\Device Parameters\Log_* ----
 * Lets us read driver progress from PowerShell without DebugView/WinDbg. */
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
    ULONG sysCfg = 0;
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

        /* Log_PipeN = addr | (type << 8) | (maxpkt << 16)   type: 2=bulk */
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

    /* ---- Milestone: register reads. Expect SYS_CFG=0x24403735 (from capture) */
    st = Rtl_Read32(ctx, REG_SYS_CFG, &sysCfg);
    RegLog(dev, L"Log_SysCfg_Status", (ULONG)st);
    RegLog(dev, L"Log_SysCfg_Value", sysCfg);
    LOG("read SYS_CFG(0x00F0) st=0x%08x val=0x%08x", st, sysCfg);

    st = Rtl_Read16(ctx, REG_9346CR, &r9346);
    RegLog(dev, L"Log_Reg0A_Status", (ULONG)st);
    RegLog(dev, L"Log_Reg0A_Value", r9346);
    LOG("read 0x000A st=0x%08x val=0x%04x", st, r9346);

    RegLog(dev, L"Log_Stage", 5);   /* finished, returning success */
    return STATUS_SUCCESS;   /* load anyway so we can inspect logs */
}
