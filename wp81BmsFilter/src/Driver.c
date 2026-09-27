/*
 * Driver entry, filter device creation and IOCTL forwarding.
 */
#include "BmsFilter.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD BmsfEvtDeviceAdd;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP BmsfEvtDriverCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BmsfEvtIoDeviceControl;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE BmsfEvtRequestCompletion;

static volatile LONG BmsfNextRequestId;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    BmsfTraceRegister();

    WDF_DRIVER_CONFIG_INIT(&config, BmsfEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = BmsfEvtDriverCleanup;

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        /* The cleanup callback does not run when the driver object is not created. */
        BmsfTraceMessage(BMSF_LEVEL_ERROR, "WdfDriverCreate failed", status);
        BmsfTraceUnregister();
        return status;
    }

    BmsfTraceMessage(BMSF_LEVEL_INFORMATION, "Driver loaded", status);
    return status;
}

static VOID
BmsfEvtDriverCleanup(
    _In_ WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    BmsfTraceMessage(BMSF_LEVEL_INFORMATION, "Driver unloading", STATUS_SUCCESS);
    BmsfTraceUnregister();
}

static NTSTATUS
BmsfEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    /*
     * As a filter, the framework forwards every request type we do not handle
     * (create, close, read, write, internal IOCTLs, PnP, power) unchanged.
     */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, BMSF_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &requestAttributes);

    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        BmsfTraceMessage(BMSF_LEVEL_ERROR, "WdfDeviceCreate failed", status);
        return status;
    }

    /*
     * Parallel dispatch so the filter adds no serialization of its own; the
     * BMS driver's sequential queue still orders the requests. Not power
     * managed, so requests are never held in the filter while the stack is
     * in a low-power state.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = BmsfEvtIoDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        BmsfTraceMessage(BMSF_LEVEL_ERROR, "WdfIoQueueCreate failed", status);
        return status;
    }

    BmsfTraceMessage(BMSF_LEVEL_INFORMATION, "Filter attached to BMS device", status);
    return status;
}

static ULONG
BmsfClampLength(
    _In_ size_t Length)
{
    return (Length > MAXULONG) ? MAXULONG : (ULONG)Length;
}

static VOID
BmsfEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PBMSF_REQUEST_CONTEXT context = BmsfGetRequestContext(Request);
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    NTSTATUS status;

    context->RequestId = (ULONG)InterlockedIncrement(&BmsfNextRequestId);
    context->IoControlCode = IoControlCode;
    context->OutputBufferLength = BmsfClampLength(OutputBufferLength);
    context->OutputBuffer = NULL;

    /*
     * METHOD_NEITHER buffers are raw user addresses, valid only in the
     * caller's context, so they are not captured. For the other methods the
     * framework returns the system buffer or maps the MDL.
     */
    if ((IoControlCode & 3) != METHOD_NEITHER) {
        if (InputBufferLength != 0 &&
            !NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, 0, &inputBuffer, NULL))) {
            inputBuffer = NULL;
        }
        if (OutputBufferLength != 0 &&
            NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 0, &outputBuffer, NULL))) {
            context->OutputBuffer = (PUCHAR)outputBuffer;
        }
    }

    /*
     * Log the input now: with METHOD_BUFFERED the input and output share the
     * same system buffer, and the BMS driver overwrites it with its output.
     */
    BmsfTraceIoctlRequest(context->RequestId, IoControlCode, (const UCHAR *)inputBuffer,
                          BmsfClampLength(InputBufferLength), context->OutputBufferLength);

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, BmsfEvtRequestCompletion, WDF_NO_CONTEXT);

    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(WdfIoQueueGetDevice(Queue)), WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(Request);
        BmsfTraceMessage(BMSF_LEVEL_ERROR, "WdfRequestSend failed", status);
        BmsfTraceIoctlCompletion(context, status, 0);
        WdfRequestComplete(Request, status);
    }
}

static VOID
BmsfEvtRequestCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PBMSF_REQUEST_CONTEXT context = BmsfGetRequestContext(Request);
    NTSTATUS status = Params->IoStatus.Status;
    ULONG_PTR information = Params->IoStatus.Information;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    /* May run at DISPATCH_LEVEL; everything below is safe there. */
    BmsfTraceIoctlCompletion(context, status, information);

    WdfRequestCompleteWithInformation(Request, status, information);
}
