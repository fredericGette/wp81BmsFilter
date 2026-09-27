/*
 * wp81BmsFilter - KMDF 1.11 upper filter for the Qualcomm PMIC BMS driver
 * (device type 0x8018, symbolic link \DosDevices\Global\QCOMPMICBMS).
 *
 * Every IRP_MJ_DEVICE_CONTROL sent to the BMS device is logged to ETW twice:
 *   - when it arrives (IOCTL name, parsed input buffer)
 *   - when the BMS driver completes it (NTSTATUS, bytes returned, parsed
 *     output buffer)
 * Both events carry the same request number so they can be paired.
 */
#pragma once

#include <wdm.h>
#include <wdf.h>

/*
 * ETW provider "WP81-BmsFilter" {965A5080-7C4B-4E1A-9E3E-52FEFE214A5C}.
 * Events are written with EtwWriteString, so no manifest is needed to read
 * them. Keywords let a session pick which events it wants.
 */
#define BMSF_KEYWORD_REQUEST     0x1ULL   /* IOCTL received, input parsed        */
#define BMSF_KEYWORD_COMPLETION  0x2ULL   /* IOCTL completed, output parsed      */
#define BMSF_KEYWORD_DIAGNOSTIC  0x4ULL   /* filter load/attach/unload, failures */

/* Standard ETW levels (TRACE_LEVEL_* is not visible to kernel code in this kit's evntrace.h). */
#define BMSF_LEVEL_ERROR         2
#define BMSF_LEVEL_WARNING       3
#define BMSF_LEVEL_INFORMATION   4

/*
 * Limits per buffer so one event always keeps its parsed part and raw part.
 * The BMS driver's own IOCTL buffers are 0-12 bytes.
 */
#define BMSF_RAW_DUMP_MAX      32   /* bytes dumped as hex             */
#define BMSF_GENERIC_DWORD_MAX 8    /* dwords decoded when layout unknown */

/*
 * Per-request state, allocated by the framework for every request delivered
 * to the filter (WdfDeviceInitSetRequestAttributes).
 */
typedef struct _BMSF_REQUEST_CONTEXT {
    ULONG  RequestId;
    ULONG  IoControlCode;
    PUCHAR OutputBuffer;        /* mapped before forwarding; NULL if not captured */
    ULONG  OutputBufferLength;
} BMSF_REQUEST_CONTEXT, *PBMSF_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BMSF_REQUEST_CONTEXT, BmsfGetRequestContext)

/*
 * Bounded wide-character text builder. Callable at any IRQL: it does not use
 * the CRT or ntstrsafe formatting routines. When the buffer fills up the text
 * ends with "..." and later appends are dropped.
 */
typedef struct _BMSF_TEXT {
    PWCHAR  Buffer;
    ULONG   Capacity;           /* in WCHARs, including the terminating NUL */
    ULONG   Length;             /* in WCHARs, excluding the terminating NUL */
    BOOLEAN Truncated;
} BMSF_TEXT, *PBMSF_TEXT;

/* Text.c */
VOID BmsfTextInit(_Out_ PBMSF_TEXT Text, _Out_writes_(Capacity) PWCHAR Buffer, _In_ ULONG Capacity);
VOID BmsfTextAppend(_Inout_ PBMSF_TEXT Text, _In_z_ PCSTR String);
VOID BmsfTextAppendUnsigned(_Inout_ PBMSF_TEXT Text, _In_ ULONG Value);
VOID BmsfTextAppendSigned(_Inout_ PBMSF_TEXT Text, _In_ LONG Value);
VOID BmsfTextAppendHex(_Inout_ PBMSF_TEXT Text, _In_ ULONG Value, _In_ ULONG Digits);
VOID BmsfTextAppendRawBytes(_Inout_ PBMSF_TEXT Text, _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
                            _In_ ULONG Length, _In_ ULONG ReturnedLength);

/* BmsIoctl.c */
VOID BmsfAppendIoctlDescription(_Inout_ PBMSF_TEXT Text, _In_ ULONG IoControlCode);
VOID BmsfAppendInput(_Inout_ PBMSF_TEXT Text, _In_ ULONG IoControlCode,
                     _In_reads_bytes_opt_(Length) const UCHAR *Buffer, _In_ ULONG Length);
VOID BmsfAppendOutput(_Inout_ PBMSF_TEXT Text, _In_ ULONG IoControlCode,
                      _In_reads_bytes_opt_(Length) const UCHAR *Buffer, _In_ ULONG Length,
                      _In_ ULONG ReturnedLength);
VOID BmsfAppendNtStatus(_Inout_ PBMSF_TEXT Text, _In_ NTSTATUS Status);

/* Trace.c */
NTSTATUS BmsfTraceRegister(VOID);
VOID BmsfTraceUnregister(VOID);
VOID BmsfTraceMessage(_In_ UCHAR Level, _In_z_ PCSTR Message, _In_ NTSTATUS Status);
VOID BmsfTraceIoctlRequest(_In_ ULONG RequestId, _In_ ULONG IoControlCode,
                           _In_reads_bytes_opt_(InputLength) const UCHAR *InputBuffer,
                           _In_ ULONG InputLength, _In_ ULONG OutputLength);
VOID BmsfTraceIoctlCompletion(_In_ const BMSF_REQUEST_CONTEXT *Context, _In_ NTSTATUS Status,
                              _In_ ULONG_PTR Information);
