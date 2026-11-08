/*++

dtmonpro.c

ARM64 KMDF 1.15 reimplementation of the Drew Technologies dtmonpro.sys
USB driver for the MongoosePro J2534 interface (USB\VID_18E1&PID_0104).

Behaviour matches the original driver as seen by user mode.

Original-derived behaviour notes:
 - On every file create the original issues a vendor control transfer
   (bRequest 0xDB, wValue 1) and always completes the create with
   STATUS_SUCCESS. On file cleanup the same request goes out with
   wValue 0.
 - The interrupt pipe runs a continuous 1-byte reader whose byte is
   stored in the device context and never consumed (faithful).
 - Deviation (deliberate): the original only logged when the bulk IN,
   bulk OUT or interrupt pipe was missing after SelectConfig. This
   build fails PrepareHardware with STATUS_INSUFFICIENT_RESOURCES
   instead of continuing with NULL pipe handles.

No WPP tracing; errors go to KdPrintEx(DPFLTR_IHVDRIVER_ID).

--*/

#include <ntddk.h>
#include <wdf.h>
#include <usbdi.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <initguid.h>
#include "public.h"

//
// Debug print: errors only, no WPP.
//
#define DT_ERR(fmt, ...) \
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
               "dtmonpro!%s: " fmt "\n", __func__, ##__VA_ARGS__))

#define DTMONPRO_MAX_TRANSFER   0x10000     // original rejects Read/Write > 64 KB
#define DTMONPRO_SERIAL_INDEX   3           // USB string descriptor for serial
#define DTMONPRO_SERIAL_LANGID  0x0409
#define DTMONPRO_S0_IDLE_MS     10000
#define DTMONPRO_REENUM_TIMEOUT_MS 5000
#define DTMONPRO_VENDOR_OPEN    0xDB        // bRequest on create/cleanup
#define DTMONPRO_VENDOR_REENUM  0xDA        // bRequest on re-enumerate IOCTL

//
// Device context. Field layout mirrors the original where it mattered
// (bulk IN / bulk OUT / interrupt pipe handles, interrupt report byte).
//
typedef struct _DEVICE_CONTEXT {

    WDFUSBDEVICE    UsbDevice;
    WDFUSBINTERFACE UsbInterface0;
    WDFUSBINTERFACE UsbInterface1;

    WDFUSBPIPE      BulkReadPipe;       // bulk IN
    WDFUSBPIPE      BulkWritePipe;      // bulk OUT
    WDFUSBPIPE      InterruptPipe;

    UCHAR           InterruptReportByte;    // last byte from the 1-byte reader
    ULONG           UsbDeviceTraits;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DtmGetDeviceContext)

//
// Callback prototypes.
//
DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD               DtmEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE         DtmEvtPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY                 DtmEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT                  DtmEvtDeviceD0Exit;
EVT_WDF_DEVICE_FILE_CREATE              DtmEvtDeviceFileCreate;
EVT_WDF_FILE_CLEANUP                    DtmEvtFileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL      DtmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_READ                DtmEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE               DtmEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_STOP                DtmEvtIoStop;
EVT_WDF_REQUEST_COMPLETION_ROUTINE      DtmEvtReadCompletion;
EVT_WDF_REQUEST_COMPLETION_ROUTINE      DtmEvtWriteCompletion;
EVT_WDF_USB_READER_COMPLETION_ROUTINE   DtmEvtInterruptReadComplete;

static
NTSTATUS
DtmSelectConfig(
    _In_ WDFDEVICE Device
    );

static
NTSTATUS
DtmSetPowerPolicy(
    _In_ WDFDEVICE Device
    );

static
NTSTATUS
DtmConfigContinuousReader(
    _In_ PDEVICE_CONTEXT Context
    );

static
NTSTATUS
DtmResetDevice(
    _In_ PDEVICE_CONTEXT Context
    );

static
VOID
DtmSendOpenRequest(
    _In_ PDEVICE_CONTEXT Context,
    _In_ USHORT Value
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DtmEvtDeviceAdd)
#pragma alloc_text(PAGE, DtmEvtPrepareHardware)
#pragma alloc_text(PAGE, DtmSelectConfig)
#pragma alloc_text(PAGE, DtmSetPowerPolicy)
#pragma alloc_text(PAGE, DtmConfigContinuousReader)
#pragma alloc_text(PAGE, DtmResetDevice)
#pragma alloc_text(PAGE, DtmSendOpenRequest)
#pragma alloc_text(PAGE, DtmEvtDeviceFileCreate)
#pragma alloc_text(PAGE, DtmEvtFileCleanup)
#pragma alloc_text(PAGE, DtmEvtIoDeviceControl)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++
    Original: WdfDriverCreate with default driver object attributes
    (SynchronizationScope None / ExecutionLevel inherit are the WDF
    defaults; the original set them explicitly to the same values).
--*/
{
    NTSTATUS            status;
    WDF_DRIVER_CONFIG   config;

    WDF_DRIVER_CONFIG_INIT(&config, DtmEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfDriverCreate failed 0x%x", status);
    }

    return status;
}

NTSTATUS
DtmEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                        status;
    WDFDEVICE                       device;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_FILEOBJECT_CONFIG           fileConfig;
    WDF_OBJECT_ATTRIBUTES           deviceAttributes;
    WDF_DEVICE_PNP_CAPABILITIES     pnpCapabilities;
    WDF_IO_QUEUE_CONFIG             queueConfig;
    WDFQUEUE                        queue;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    //
    // PnP/power callbacks: PrepareHardware, D0Entry, D0Exit.
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = DtmEvtPrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = DtmEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = DtmEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    //
    // File object callbacks: the original registers create + cleanup
    // (no close), FileObjectClass WdfFileObjectWdfCannotUseFsContexts,
    // AutoForwardCleanupClose default.
    //
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                                DtmEvtDeviceFileCreate,
                                WDF_NO_EVENT_CALLBACK,     // no EvtFileClose
                                DtmEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit,
                                     &fileConfig,
                                     WDF_NO_OBJECT_ATTRIBUTES);

    //
    // One open handle at a time; buffered I/O.
    //
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfDeviceCreate failed 0x%x", status);
        return status;
    }

    //
    // PnP capabilities: SurpriseRemovalOK = TRUE, everything else default
    // (INIT macro already leaves Address/UINumber at 0xFFFFFFFF).
    //
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCapabilities);
    pnpCapabilities.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &pnpCapabilities);

    //
    // Default queue: parallel dispatch, DeviceControl only.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = DtmEvtIoDeviceControl;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              NULL);
    if (!NT_SUCCESS(status)) {
        DT_ERR("default queue create failed 0x%x", status);
        return status;
    }

    //
    // Read queue: sequential, routed read requests.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoRead = DtmEvtIoRead;
    queueConfig.EvtIoStop = DtmEvtIoStop;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &queue);
    if (!NT_SUCCESS(status)) {
        DT_ERR("read queue create failed 0x%x", status);
        return status;
    }

    status = WdfDeviceConfigureRequestDispatching(device,
                                                  queue,
                                                  WdfRequestTypeRead);
    if (!NT_SUCCESS(status)) {
        DT_ERR("read queue dispatch failed 0x%x", status);
        return status;
    }

    //
    // Write queue: sequential, routed write requests.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoWrite = DtmEvtIoWrite;
    queueConfig.EvtIoStop = DtmEvtIoStop;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &queue);
    if (!NT_SUCCESS(status)) {
        DT_ERR("write queue create failed 0x%x", status);
        return status;
    }

    status = WdfDeviceConfigureRequestDispatching(device,
                                                  queue,
                                                  WdfRequestTypeWrite);
    if (!NT_SUCCESS(status)) {
        DT_ERR("write queue dispatch failed 0x%x", status);
        return status;
    }

    //
    // Device interface GUID - this is what monpj432.dll opens.
    //
    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_DTMONPRO,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfDeviceCreateDeviceInterface failed 0x%x", status);
        return status;
    }

    return status;
}

NTSTATUS
DtmEvtPrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS                    status;
    PDEVICE_CONTEXT             ctx;
    WDF_USB_DEVICE_INFORMATION  usbInfo;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    ctx = DtmGetDeviceContext(Device);

    //
    // Create the USB target device (only once across D0 cycles).
    //
    if (ctx->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(Device,
                                          WDF_NO_OBJECT_ATTRIBUTES,
                                          &ctx->UsbDevice);
        if (!NT_SUCCESS(status)) {
            DT_ERR("WdfUsbTargetDeviceCreate failed 0x%x", status);
            return status;
        }
    }

    //
    // Device traits (remote wake decides whether S0 idle / Sx wake
    // policy is applied, exactly like the original).
    //
    WDF_USB_DEVICE_INFORMATION_INIT(&usbInfo);
    status = WdfUsbTargetDeviceRetrieveInformation(ctx->UsbDevice, &usbInfo);
    if (NT_SUCCESS(status)) {
        ctx->UsbDeviceTraits = usbInfo.Traits;
    }

    //
    // Select configuration: interfaces 0 and 1, alternate setting 0,
    // then record the bulk IN / bulk OUT / interrupt pipes.
    // DEVIATION: original only logged a missing pipe; we fail.
    //
    status = DtmSelectConfig(Device);
    if (!NT_SUCCESS(status)) {
        DT_ERR("SelectConfig/pipe discovery failed 0x%x", status);
        return status;
    }

    if (ctx->UsbDeviceTraits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE) {
        status = DtmSetPowerPolicy(Device);
        if (!NT_SUCCESS(status)) {
            DT_ERR("power policy failed 0x%x", status);
            return status;
        }
    }

    status = DtmConfigContinuousReader(ctx);
    if (!NT_SUCCESS(status)) {
        DT_ERR("continuous reader failed 0x%x", status);
        return status;
    }

    return status;
}

static
NTSTATUS
DtmSelectConfig(
    _In_ WDFDEVICE Device
    )
/*++
    Select config with interfaces 0 and 1 at alternate setting 0, then
    walk every configured pipe: interrupt -> InterruptPipe, bulk IN ->
    BulkReadPipe, bulk OUT -> BulkWritePipe. Every pipe gets
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck.
--*/
{
    NTSTATUS                                status;
    PDEVICE_CONTEXT                         ctx;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS     configParams;
    WDF_USB_INTERFACE_SETTING_PAIR          pairs[2];
    UCHAR                                   numInterfaces;
    UCHAR                                   i;
    UCHAR                                   j;

    PAGED_CODE();

    ctx = DtmGetDeviceContext(Device);

    pairs[0].UsbInterface = WdfUsbTargetDeviceGetInterface(ctx->UsbDevice, 0);
    pairs[0].SettingIndex = 0;
    pairs[1].UsbInterface = WdfUsbTargetDeviceGetInterface(ctx->UsbDevice, 1);
    pairs[1].SettingIndex = 0;

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
        &configParams, 2, pairs);

    status = WdfUsbTargetDeviceSelectConfig(ctx->UsbDevice,
                                            WDF_NO_OBJECT_ATTRIBUTES,
                                            &configParams);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfUsbTargetDeviceSelectConfig failed 0x%x", status);
        return status;
    }

    numInterfaces =
        configParams.Types.MultiInterface.NumberOfConfiguredInterfaces;

    for (i = 0; i < numInterfaces; i++) {
        WDFUSBINTERFACE usbInterface = pairs[i].UsbInterface;
        UCHAR           numPipes;

        numPipes = WdfUsbInterfaceGetNumConfiguredPipes(usbInterface);

        for (j = 0; j < numPipes; j++) {
            WDFUSBPIPE                  pipe;
            WDF_USB_PIPE_INFORMATION    pipeInfo;

            WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
            pipe = WdfUsbInterfaceGetConfiguredPipe(usbInterface,
                                                    j,
                                                    &pipeInfo);

            WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

            if (pipeInfo.PipeType == WdfUsbPipeTypeInterrupt) {
                ctx->InterruptPipe = pipe;
            }
            else if (pipeInfo.PipeType == WdfUsbPipeTypeBulk) {
                if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
                    ctx->BulkReadPipe = pipe;
                }
                if (WdfUsbTargetPipeIsOutEndpoint(pipe)) {
                    ctx->BulkWritePipe = pipe;
                }
            }
        }
    }

    //
    // DEVIATION from the original: it logged STATUS_INSUFFICIENT_RESOURCES
    // and continued with NULL handles; we fail PrepareHardware instead.
    //
    if (ctx->BulkReadPipe == NULL ||
        ctx->BulkWritePipe == NULL ||
        ctx->InterruptPipe == NULL) {
        DT_ERR("required pipe missing (in %p out %p int %p)",
               ctx->BulkReadPipe, ctx->BulkWritePipe, ctx->InterruptPipe);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
DtmSetPowerPolicy(
    _In_ WDFDEVICE Device
    )
/*++
    Applied only when the device is remote-wake capable, exactly like the
    original: S0 selective suspend with 10 s idle timeout and user control;
    Sx wake with user control.
--*/
{
    NTSTATUS                            status;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS wakeSettings;

    PAGED_CODE();

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings,
                                               IdleUsbSelectiveSuspend);
    idleSettings.IdleTimeout = DTMONPRO_S0_IDLE_MS;

    status = WdfDeviceAssignS0IdleSettings(Device, &idleSettings);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfDeviceAssignS0IdleSettings failed 0x%x", status);
        return status;
    }

    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&wakeSettings);

    status = WdfDeviceAssignSxWakeSettings(Device, &wakeSettings);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfDeviceAssignSxWakeSettings failed 0x%x", status);
        return status;
    }

    return status;
}

static
NTSTATUS
DtmConfigContinuousReader(
    _In_ PDEVICE_CONTEXT Context
    )
/*++
    Continuous reader on the interrupt pipe: 1-byte transfers, byte is
    stored in the context and never consumed (faithful to the original).
    NumPendingReads left at the framework default.
--*/
{
    NTSTATUS                        status;
    WDF_USB_CONTINUOUS_READER_CONFIG contConfig;

    PAGED_CODE();

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&contConfig,
                                          DtmEvtInterruptReadComplete,
                                          Context,
                                          sizeof(UCHAR));

    status = WdfUsbTargetPipeConfigContinuousReader(Context->InterruptPipe,
                                                    &contConfig);
    if (!NT_SUCCESS(status)) {
        DT_ERR("WdfUsbTargetPipeConfigContinuousReader failed 0x%x", status);
    }

    return status;
}

NTSTATUS
DtmEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(PreviousState);

    return WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(DtmGetDeviceContext(Device)->InterruptPipe));
}

NTSTATUS
DtmEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(TargetState);

    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(DtmGetDeviceContext(Device)->InterruptPipe),
        WdfIoTargetCancelSentIo);

    return STATUS_SUCCESS;
}

//
// ------------------------- File create / cleanup -------------------------
//

static
VOID
DtmSendOpenRequest(
    _In_ PDEVICE_CONTEXT Context,
    _In_ USHORT          Value
    )
/*++
    Vendor control transfer, no data stage, no timeout:
      bmRequestType 0x40 (host-to-device, vendor, device)
      bRequest 0xDB, wValue = Value, wIndex 0, wLength 0.
    Sent on every file create (Value 1) and file cleanup (Value 0).
    Result is intentionally ignored by the caller, as in the original.
--*/
{
    NTSTATUS                        status;
    WDF_USB_CONTROL_SETUP_PACKET    packet;

    PAGED_CODE();

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&packet,
                                             BmRequestHostToDevice,
                                             BmRequestToDevice,
                                             DTMONPRO_VENDOR_OPEN,
                                             Value,
                                             0);

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        Context->UsbDevice,
        WDF_NO_HANDLE,      // no preallocated request
        NULL,               // no send options => no timeout
        &packet,
        NULL,               // no data stage
        NULL);

    if (!NT_SUCCESS(status)) {
        DT_ERR("open/close control transfer (wValue %u) failed 0x%x",
               Value, status);
    }
}

VOID
DtmEvtDeviceFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    UNREFERENCED_PARAMETER(FileObject);
    PAGED_CODE();

    DtmSendOpenRequest(DtmGetDeviceContext(Device), 1);

    //
    // Original always completes the create successfully.
    //
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
DtmEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device;

    PAGED_CODE();

    device = WdfFileObjectGetDevice(FileObject);
    DtmSendOpenRequest(DtmGetDeviceContext(device), 0);
}

//
// ------------------------------ Read / Write ------------------------------
//

VOID
DtmEvtIoRead(
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t    Length
    )
/*++
    ReadFile -> bulk IN pipe. Max transfer 0x10000 like the original;
    larger requests fail with STATUS_INVALID_PARAMETER.
--*/
{
    NTSTATUS        status;
    PDEVICE_CONTEXT ctx;
    WDFUSBPIPE      pipe;
    WDFMEMORY       mem;

    ctx  = DtmGetDeviceContext(WdfIoQueueGetDevice(Queue));
    pipe = ctx->BulkReadPipe;

    if (Length > DTMONPRO_MAX_TRANSFER) {
        DT_ERR("read length 0x%Ix > 0x%x", Length, DTMONPRO_MAX_TRANSFER);
        status = STATUS_INVALID_PARAMETER;
        goto Fail;
    }

    status = WdfRequestRetrieveOutputMemory(Request, &mem);
    if (!NT_SUCCESS(status)) {
        DT_ERR("RetrieveOutputMemory failed 0x%x", status);
        goto Fail;
    }

    status = WdfUsbTargetPipeFormatRequestForRead(pipe, Request, mem, NULL);
    if (!NT_SUCCESS(status)) {
        DT_ERR("FormatRequestForRead failed 0x%x", status);
        goto Fail;
    }

    WdfRequestSetCompletionRoutine(Request,
                                   DtmEvtReadCompletion,
                                   pipe);

    if (WdfRequestSend(Request, WdfUsbTargetPipeGetIoTarget(pipe), WDF_NO_SEND_OPTIONS) == FALSE) {
        status = WdfRequestGetStatus(Request);
        DT_ERR("WdfRequestSend (read) failed 0x%x", status);
        goto Fail;
    }

    return;

Fail:
    WdfRequestCompleteWithInformation(Request, status, 0);
}

VOID
DtmEvtReadCompletion(
    _In_ WDFREQUEST                      Request,
    _In_ WDFIOTARGET                     Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
    _In_ WDFCONTEXT                      Context
    )
/*++
    Complete with the USB completion status and the byte count the pipe
    target reported in PipeRead.Length.
--*/
{
    NTSTATUS status;
    size_t   bytesRead;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    status    = Params->IoStatus.Status;
    bytesRead = Params->Parameters.Usb.Completion->Parameters.PipeRead.Length;

    WdfRequestCompleteWithInformation(Request, status, bytesRead);
}

VOID
DtmEvtIoWrite(
    _In_ WDFQUEUE  Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t    Length
    )
/*++
    WriteFile -> bulk OUT pipe. Same 0x10000 cap as the original.
--*/
{
    NTSTATUS        status;
    PDEVICE_CONTEXT ctx;
    WDFUSBPIPE      pipe;
    WDFMEMORY       mem;

    ctx  = DtmGetDeviceContext(WdfIoQueueGetDevice(Queue));
    pipe = ctx->BulkWritePipe;

    if (Length > DTMONPRO_MAX_TRANSFER) {
        DT_ERR("write length 0x%Ix > 0x%x", Length, DTMONPRO_MAX_TRANSFER);
        status = STATUS_INVALID_PARAMETER;
        goto Fail;
    }

    status = WdfRequestRetrieveInputMemory(Request, &mem);
    if (!NT_SUCCESS(status)) {
        DT_ERR("RetrieveInputMemory failed 0x%x", status);
        goto Fail;
    }

    status = WdfUsbTargetPipeFormatRequestForWrite(pipe, Request, mem, NULL);
    if (!NT_SUCCESS(status)) {
        DT_ERR("FormatRequestForWrite failed 0x%x", status);
        goto Fail;
    }

    WdfRequestSetCompletionRoutine(Request,
                                   DtmEvtWriteCompletion,
                                   pipe);

    if (WdfRequestSend(Request, WdfUsbTargetPipeGetIoTarget(pipe), WDF_NO_SEND_OPTIONS) == FALSE) {
        status = WdfRequestGetStatus(Request);
        DT_ERR("WdfRequestSend (write) failed 0x%x", status);
        goto Fail;
    }

    return;

Fail:
    WdfRequestCompleteWithInformation(Request, status, 0);
}

VOID
DtmEvtWriteCompletion(
    _In_ WDFREQUEST                      Request,
    _In_ WDFIOTARGET                     Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
    _In_ WDFCONTEXT                      Context
    )
{
    NTSTATUS status;
    size_t   bytesWritten;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    status       = Params->IoStatus.Status;
    bytesWritten = Params->Parameters.Usb.Completion->Parameters.PipeWrite.Length;

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}

VOID
DtmEvtIoStop(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG      ActionFlags
    )
/*++
    Same as the original: Suspend -> stop-acknowledge without requeue;
    Purge -> cancel the sent USB request.
--*/
{
    UNREFERENCED_PARAMETER(Queue);

    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
    }
    else if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestCancelSentRequest(Request);
    }
}

//
// ------------------------- Interrupt continuous reader -------------------------
//

VOID
DtmEvtInterruptReadComplete(
    _In_ WDFUSBPIPE Pipe,
    _In_ WDFMEMORY  Buffer,
    _In_ size_t     NumBytesTransferred,
    _In_ WDFCONTEXT Context
    )
/*++
    Stores the received byte in the device context. Nothing consumes it
    (the original behaves the same).
--*/
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred == 0) {
        return;
    }

    ctx->InterruptReportByte = *((PUCHAR)WdfMemoryGetBuffer(Buffer, NULL));
}

//
// --------------------------------- IOCTLs ---------------------------------
//

static
NTSTATUS
DtmResetDevice(
    _In_ PDEVICE_CONTEXT Context
    )
/*++
    Stop interrupt + bulk IN + bulk OUT targets (cancel sent I/O), reset
    the port, restart all three. Returns the reset status, like the
    original.
--*/
{
    NTSTATUS status;

    PAGED_CODE();

    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Context->InterruptPipe), WdfIoTargetCancelSentIo);
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Context->BulkReadPipe),  WdfIoTargetCancelSentIo);
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Context->BulkWritePipe), WdfIoTargetCancelSentIo);

    status = WdfUsbTargetDeviceResetPortSynchronously(Context->UsbDevice);
    if (!NT_SUCCESS(status)) {
        DT_ERR("ResetPortSynchronously failed 0x%x", status);
    }

    WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Context->InterruptPipe));
    WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Context->BulkReadPipe));
    WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Context->BulkWritePipe));

    return status;
}

VOID
DtmEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS                        status;
    ULONG_PTR                       info;
    WDFDEVICE                       device;
    PDEVICE_CONTEXT                 ctx;
    PVOID                           buffer;
    USHORT                          size;
    USHORT                          numChars;
    WDF_USB_CONTROL_SETUP_PACKET    packet;
    WDF_REQUEST_SEND_OPTIONS        sendOptions;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    PAGED_CODE();

    device = WdfIoQueueGetDevice(Queue);
    ctx    = DtmGetDeviceContext(device);
    info   = 0;

    switch (IoControlCode) {

    case IOCTL_DTMONPRO_GET_CONFIG_DESCRIPTOR:
        //
        // Original requires the first (sizing) call to return
        // STATUS_BUFFER_TOO_SMALL; anything else is propagated.
        //
        size   = 0;
        status = WdfUsbTargetDeviceRetrieveConfigDescriptor(ctx->UsbDevice,
                                                            NULL,
                                                            &size);
        if (status != STATUS_BUFFER_TOO_SMALL) {
            DT_ERR("RetrieveConfigDescriptor(size) 0x%x", status);
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request, size, &buffer, NULL);
        if (!NT_SUCCESS(status)) {
            DT_ERR("RetrieveOutputBuffer failed 0x%x", status);
            break;
        }

        status = WdfUsbTargetDeviceRetrieveConfigDescriptor(ctx->UsbDevice,
                                                            buffer,
                                                            &size);
        if (!NT_SUCCESS(status)) {
            DT_ERR("RetrieveConfigDescriptor failed 0x%x", status);
            break;
        }
        info = size;
        break;

    case IOCTL_DTMONPRO_GET_CONSTANTS:
        //
        // 8 constant bytes: two ULONGs of 1.
        //
        status = WdfRequestRetrieveOutputBuffer(Request, 8, &buffer, NULL);
        if (!NT_SUCCESS(status)) {
            DT_ERR("RetrieveOutputBuffer failed 0x%x", status);
            break;
        }
        ((PULONG)buffer)[0] = 1;
        ((PULONG)buffer)[1] = 1;
        info = 8;
        break;

    case IOCTL_DTMONPRO_GET_SERIAL_NUMBER:
        //
        // USB string descriptor index 3, LANGID 0x0409, UTF-16.
        // First call sizes it, second fills the caller's output buffer.
        //
        numChars = 0;
        WdfUsbTargetDeviceQueryString(ctx->UsbDevice,
                                      WDF_NO_HANDLE,
                                      NULL,
                                      NULL,
                                      &numChars,
                                      DTMONPRO_SERIAL_INDEX,
                                      DTMONPRO_SERIAL_LANGID);

        status = WdfRequestRetrieveOutputBuffer(Request,
                                                (size_t)numChars * sizeof(WCHAR),
                                                &buffer,
                                                NULL);
        if (!NT_SUCCESS(status)) {
            DT_ERR("RetrieveOutputBuffer failed 0x%x", status);
            break;
        }

        status = WdfUsbTargetDeviceQueryString(ctx->UsbDevice,
                                               WDF_NO_HANDLE,
                                               NULL,
                                               (PUSHORT)buffer,
                                               &numChars,
                                               DTMONPRO_SERIAL_INDEX,
                                               DTMONPRO_SERIAL_LANGID);
        if (!NT_SUCCESS(status)) {
            DT_ERR("QueryString failed 0x%x", status);
            break;
        }
        info = (ULONG_PTR)numChars * sizeof(WCHAR);
        break;

    case IOCTL_DTMONPRO_RESET_PORT:
        status = DtmResetDevice(ctx);
        break;

    case IOCTL_DTMONPRO_REENUMERATE:
        //
        // Vendor control transfer, no data stage, 5 s timeout:
        // bmRequestType 0x40, bRequest 0xDA, wValue/wIndex/wLength 0.
        //
        WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&packet,
                                                 BmRequestHostToDevice,
                                                 BmRequestToDevice,
                                                 DTMONPRO_VENDOR_REENUM,
                                                 0,
                                                 0);

        WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, 0);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
            &sendOptions, WDF_REL_TIMEOUT_IN_MS(DTMONPRO_REENUM_TIMEOUT_MS));

        status = WdfUsbTargetDeviceSendControlTransferSynchronously(
            ctx->UsbDevice,
            WDF_NO_HANDLE,
            &sendOptions,
            &packet,
            NULL,
            NULL);
        if (!NT_SUCCESS(status)) {
            DT_ERR("re-enumerate transfer failed 0x%x", status);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}
