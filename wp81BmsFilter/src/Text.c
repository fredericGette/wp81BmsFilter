/*
 * Bounded wide-character text builder used to compose the ETW messages.
 * No CRT or ntstrsafe formatting, so it is safe at DISPATCH_LEVEL.
 */
#include "BmsFilter.h"

/* WCHARs kept free for the "..." written when the text is truncated. */
#define BMSF_TEXT_ELLIPSIS_CHARS 3

static const CHAR BmsfHexDigits[] = "0123456789ABCDEF";

VOID
BmsfTextInit(
    _Out_ PBMSF_TEXT Text,
    _Out_writes_(Capacity) PWCHAR Buffer,
    _In_ ULONG Capacity)
{
    NT_ASSERT(Capacity > BMSF_TEXT_ELLIPSIS_CHARS + 1);

    Text->Buffer = Buffer;
    Text->Capacity = Capacity;
    Text->Length = 0;
    Text->Truncated = FALSE;
    Buffer[0] = L'\0';
}

static VOID
BmsfTextPutChar(
    _Inout_ PBMSF_TEXT Text,
    _In_ CHAR Char)
{
    ULONG i;

    if (Text->Truncated) {
        return;
    }

    if (Text->Length + BMSF_TEXT_ELLIPSIS_CHARS + 1 < Text->Capacity) {
        Text->Buffer[Text->Length++] = (WCHAR)(UCHAR)Char;
        Text->Buffer[Text->Length] = L'\0';
        return;
    }

    Text->Truncated = TRUE;
    for (i = 0; i < BMSF_TEXT_ELLIPSIS_CHARS; i++) {
        Text->Buffer[Text->Length++] = L'.';
    }
    Text->Buffer[Text->Length] = L'\0';
}

VOID
BmsfTextAppend(
    _Inout_ PBMSF_TEXT Text,
    _In_z_ PCSTR String)
{
    while (*String != '\0') {
        BmsfTextPutChar(Text, *String++);
    }
}

VOID
BmsfTextAppendUnsigned(
    _Inout_ PBMSF_TEXT Text,
    _In_ ULONG Value)
{
    CHAR digits[10];
    ULONG count = 0;

    do {
        digits[count++] = (CHAR)('0' + (Value % 10));
        Value /= 10;
    } while (Value != 0);

    while (count != 0) {
        BmsfTextPutChar(Text, digits[--count]);
    }
}

VOID
BmsfTextAppendSigned(
    _Inout_ PBMSF_TEXT Text,
    _In_ LONG Value)
{
    if (Value < 0) {
        BmsfTextPutChar(Text, '-');
        BmsfTextAppendUnsigned(Text, 0u - (ULONG)Value);
    } else {
        BmsfTextAppendUnsigned(Text, (ULONG)Value);
    }
}

/* Appends "0x" followed by exactly Digits hex digits (1-8). */
VOID
BmsfTextAppendHex(
    _Inout_ PBMSF_TEXT Text,
    _In_ ULONG Value,
    _In_ ULONG Digits)
{
    if (Digits == 0 || Digits > 8) {
        Digits = 8;
    }

    BmsfTextAppend(Text, "0x");
    while (Digits != 0) {
        Digits--;
        BmsfTextPutChar(Text, BmsfHexDigits[(Value >> (Digits * 4)) & 0xF]);
    }
}

/*
 * Appends up to BMSF_RAW_DUMP_MAX bytes as "AA BB CC". When ReturnedLength is
 * smaller than Length, a "|" marks where the bytes returned to the caller end.
 */
VOID
BmsfTextAppendRawBytes(
    _Inout_ PBMSF_TEXT Text,
    _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_ ULONG ReturnedLength)
{
    ULONG count;
    ULONG i;

    if (Length == 0) {
        BmsfTextAppend(Text, "(none)");
        return;
    }
    if (Buffer == NULL) {
        BmsfTextAppend(Text, "(not captured)");
        return;
    }

    count = (Length < BMSF_RAW_DUMP_MAX) ? Length : BMSF_RAW_DUMP_MAX;
    for (i = 0; i < count; i++) {
        if (i != 0) {
            BmsfTextPutChar(Text, ' ');
        }
        if (i == ReturnedLength) {
            BmsfTextAppend(Text, "| ");
        }
        BmsfTextPutChar(Text, BmsfHexDigits[Buffer[i] >> 4]);
        BmsfTextPutChar(Text, BmsfHexDigits[Buffer[i] & 0xF]);
    }

    if (count < Length) {
        BmsfTextAppend(Text, " ... (+");
        BmsfTextAppendUnsigned(Text, Length - count);
        BmsfTextAppend(Text, " bytes)");
    }
}
