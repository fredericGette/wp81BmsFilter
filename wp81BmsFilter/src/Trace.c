/*
 * ETW provider "WP81-BmsFilter" {965A5080-7C4B-4E1A-9E3E-52FEFE214A5C}.
 *
 * Events are plain strings (EtwWriteString): the Windows 8.1 kernel has no
 * TraceLogging support, and a manifest-based provider would need mc.exe to
 * embed the manifest resource. Any ETW consumer (tracerpt, WPA, xperf,
 * PerfView) shows these strings without a manifest.
 *
 *   Level        Keyword         Event
 *   Information  REQUEST    0x1  "REQ #n ..."  IOCTL received, input parsed
 *   Information  COMPLETION 0x2  "CPL #n ..."  IOCTL completed successfully
 *   Warning      COMPLETION 0x2  "CPL #n ..."  IOCTL completed with an error status
 *   Info/Error   DIAGNOSTIC 0x4  filter lifecycle and internal failures
 */
#include "BmsFilter.h"

/* {965A5080-7C4B-4E1A-9E3E-52FEFE214A5C} */
static const GUID BmsfProviderGuid =
    { 0x965a5080, 0x7c4b, 0x4e1a, { 0x9e, 0x3e, 0x52, 0xfe, 0xfe, 0x21, 0x4a, 0x5c } };

/*
 * 1.25 KB of stack. The completion routine can run nested inside the BMS
 * driver's own call chain (synchronous completion), so this stays modest.
 * Known BMS IOCTLs need ~350 characters; an oversized unknown one ends in "...".
 */
#define BMSF_TRACE_CHARS 640

static REGHANDLE BmsfEtwHandle;
static BOOLEAN BmsfEtwRegistered;

NTSTATUS
BmsfTraceRegister(VOID)
{
    NTSTATUS status = EtwRegister(&BmsfProviderGuid, NULL, NULL, &BmsfEtwHandle);

    /* Logging is best effort: a registration failure must not stop the filter. */
    BmsfEtwRegistered = NT_SUCCESS(status);
    return status;
}

VOID
BmsfTraceUnregister(VOID)
{
    if (BmsfEtwRegistered) {
        BmsfEtwRegistered = FALSE;
        EtwUnregister(BmsfEtwHandle);
    }
}

static BOOLEAN
BmsfTraceEnabled(
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    return BmsfEtwRegistered && EtwProviderEnabled(BmsfEtwHandle, Level, Keyword);
}

static VOID
BmsfTraceWrite(
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ const BMSF_TEXT *Text)
{
    EtwWriteString(BmsfEtwHandle, Level, Keyword, NULL, Text->Buffer);
}

VOID
BmsfTraceMessage(
    _In_ UCHAR Level,
    _In_z_ PCSTR Message,
    _In_ NTSTATUS Status)
{
    WCHAR buffer[BMSF_TRACE_CHARS];
    BMSF_TEXT text;

    if (!BmsfTraceEnabled(Level, BMSF_KEYWORD_DIAGNOSTIC)) {
        return;
    }

    BmsfTextInit(&text, buffer, ARRAYSIZE(buffer));
    BmsfTextAppend(&text, "BmsFilter: ");
    BmsfTextAppend(&text, Message);
    BmsfTextAppend(&text, ", status=");
    BmsfAppendNtStatus(&text, Status);

    BmsfTraceWrite(Level, BMSF_KEYWORD_DIAGNOSTIC, &text);
}

/*
 * REQ #12 IOCTL_BMS_SET_SYSTEM_INFO
 *   in=12 out=4 | Input: Timestamp?=..., Temperature?=25, BatteryId?=... | InputRaw: ...
 */
VOID
BmsfTraceIoctlRequest(
    _In_ ULONG RequestId,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) const UCHAR *InputBuffer,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    WCHAR buffer[BMSF_TRACE_CHARS];
    BMSF_TEXT text;

    if (!BmsfTraceEnabled(BMSF_LEVEL_INFORMATION, BMSF_KEYWORD_REQUEST)) {
        return;
    }

    BmsfTextInit(&text, buffer, ARRAYSIZE(buffer));
    BmsfTextAppend(&text, "REQ #");
    BmsfTextAppendUnsigned(&text, RequestId);
    BmsfTextAppend(&text, " ");
    BmsfAppendIoctlDescription(&text, IoControlCode);
    BmsfTextAppend(&text, " in=");
    BmsfTextAppendUnsigned(&text, InputLength);
    BmsfTextAppend(&text, " out=");
    BmsfTextAppendUnsigned(&text, OutputLength);
    BmsfTextAppend(&text, " | Input: ");
    BmsfAppendInput(&text, IoControlCode, InputBuffer, InputLength);
    BmsfTextAppend(&text, " | InputRaw: ");
    BmsfTextAppendRawBytes(&text, InputBuffer, InputLength, InputLength);

    BmsfTraceWrite(BMSF_LEVEL_INFORMATION, BMSF_KEYWORD_REQUEST, &text);
}

/*
 * CPL #12 IOCTL_BMS_SET_SYSTEM_INFO status=0x00000000 STATUS_SUCCESS
 *   returned=4/4 | Output: Result?=0 | OutputRaw: 00 00 00 00
 */
VOID
BmsfTraceIoctlCompletion(
    _In_ const BMSF_REQUEST_CONTEXT *Context,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    WCHAR buffer[BMSF_TRACE_CHARS];
    BMSF_TEXT text;
    UCHAR level = NT_ERROR(Status) ? BMSF_LEVEL_WARNING : BMSF_LEVEL_INFORMATION;
    ULONG returned;

    if (!BmsfTraceEnabled(level, BMSF_KEYWORD_COMPLETION)) {
        return;
    }

    returned = (Information < Context->OutputBufferLength) ? (ULONG)Information : Context->OutputBufferLength;

    BmsfTextInit(&text, buffer, ARRAYSIZE(buffer));
    BmsfTextAppend(&text, "CPL #");
    BmsfTextAppendUnsigned(&text, Context->RequestId);
    BmsfTextAppend(&text, " ");
    BmsfAppendIoctlDescription(&text, Context->IoControlCode);
    BmsfTextAppend(&text, " status=");
    BmsfAppendNtStatus(&text, Status);
    BmsfTextAppend(&text, " returned=");
    BmsfTextAppendUnsigned(&text, (Information > MAXULONG) ? MAXULONG : (ULONG)Information);
    BmsfTextAppend(&text, "/");
    BmsfTextAppendUnsigned(&text, Context->OutputBufferLength);

    if (NT_ERROR(Status)) {
        /* On error the buffer content is not meaningful; show only what was reported. */
        BmsfTextAppend(&text, " | Output: (not parsed, request failed) | OutputRaw: ");
        BmsfTextAppendRawBytes(&text, Context->OutputBuffer, returned, returned);
    } else {
        /*
         * The whole output buffer is parsed, not just the returned bytes: some
         * BMS handlers write more than they report (GET_INTERNAL_CALC writes 12
         * bytes but returns 4). Bytes past "returned" are marked.
         */
        BmsfTextAppend(&text, " | Output: ");
        BmsfAppendOutput(&text, Context->IoControlCode, Context->OutputBuffer,
                         Context->OutputBufferLength, returned);
        BmsfTextAppend(&text, " | OutputRaw: ");
        BmsfTextAppendRawBytes(&text, Context->OutputBuffer, Context->OutputBufferLength, returned);
    }

    BmsfTraceWrite(level, BMSF_KEYWORD_COMPLETION, &text);
}
