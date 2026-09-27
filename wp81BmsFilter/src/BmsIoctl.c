/*
 * IOCTL knowledge base for the PMIC BMS driver: names, code decoding and
 * buffer layouts, taken from the reverse-engineering notes
 * (pmic_bms_driver_functions.txt).
 *
 * All BMS buffers seen so far are arrays of 32-bit values, so a layout is a
 * list of named dwords. A field name ending in '?' is a layout inferred from
 * the backend's signature but not confirmed against the dispatch code.
 */
#include "BmsFilter.h"

typedef enum _BMSF_FIELD_KIND {
    BmsfFieldUnsigned,
    BmsfFieldSigned,
    BmsfFieldHex,
    BmsfFieldXoadcCode,         /* raw XOADC code, also shows the value the driver stores */
    BmsfFieldChargingState,     /* value followed by its meaning */
} BMSF_FIELD_KIND;

typedef struct _BMSF_FIELD {
    PCSTR Name;
    BMSF_FIELD_KIND Kind;
} BMSF_FIELD;

/* Fields == NULL: layout unknown, the buffer is dumped as generic dwords. */
typedef struct _BMSF_LAYOUT {
    const BMSF_FIELD *Fields;
    ULONG Count;
} BMSF_LAYOUT;

typedef struct _BMSF_IOCTL_INFO {
    ULONG Code;
    PCSTR Name;
    BMSF_LAYOUT Input;
    BMSF_LAYOUT Output;
} BMSF_IOCTL_INFO;

#define BMSF_LAYOUT_OF(fields)  { fields, ARRAYSIZE(fields) }
#define BMSF_LAYOUT_NONE        { BmsfNoFields, 0 }
#define BMSF_LAYOUT_UNKNOWN     { NULL, 0 }

static const BMSF_FIELD BmsfNoFields[1] = { { NULL, BmsfFieldUnsigned } };

/* IOCTL_BMS_GET_BATTERY_CHARGING_PROFILE: mapping to the "return 4" handler is a guess. */
static const BMSF_FIELD BmsfChargingProfileOut[] = {
    { "ChargingProfile?", BmsfFieldUnsigned },
};

/* IOCTL_BMS_GET_BATTERY_CURRENT: calibrated current (PmicBmsReadCalibratedCurrentSeed). */
static const BMSF_FIELD BmsfCurrentOut[] = {
    { "CurrentMa?", BmsfFieldSigned },
};

/* IOCTL_BMS_GET_BATTERY_VOLTAGE: selector 6 conversion, ~0.0977 unit per LSB above 0x6000. */
static const BMSF_FIELD BmsfVoltageOut[] = {
    { "Voltage", BmsfFieldUnsigned },
};

/* IOCTL_BMS_GET_PERCENT_CHARGE: SOC as produced by PmicBmsEstimateAndRateLimitSoc. */
static const BMSF_FIELD BmsfPercentChargeOut[] = {
    { "Soc", BmsfFieldUnsigned },
};

/* IOCTL_BMS_SET_CHARGING_STATE: confirmed, one LONG read by PmicBmsIoctlSetChargingState. */
static const BMSF_FIELD BmsfChargingStateIn[] = {
    { "NewChargingState", BmsfFieldChargingState },
};

/* IOCTL_BMS_SET_SYSTEM_INFO: order taken from PmicBmsIoctlSetSystemInfo's parameters. */
static const BMSF_FIELD BmsfSystemInfoIn[] = {
    { "Timestamp?",   BmsfFieldUnsigned },
    { "Temperature?", BmsfFieldSigned },
    { "BatteryId?",   BmsfFieldUnsigned },
};

static const BMSF_FIELD BmsfSystemInfoOut[] = {
    { "Result?", BmsfFieldSigned },
};

/* IOCTL_BMS_SET_XOADC_CAL_VAL: confirmed, two adjacent input dwords. */
static const BMSF_FIELD BmsfXoadcCalIn[] = {
    { "RawPointA", BmsfFieldXoadcCode },
    { "RawPointB", BmsfFieldXoadcCode },
};

/* IOCTL_BMS_GET_INTERNAL_CALC: confirmed, three ULONGs (driver reports only 4 bytes returned). */
static const BMSF_FIELD BmsfInternalCalcOut[] = {
    { "DeratedFcc",         BmsfFieldUnsigned },
    { "RemainingChargeRef", BmsfFieldUnsigned },
    { "Headroom",           BmsfFieldUnsigned },
};

static const BMSF_IOCTL_INFO BmsfIoctlTable[] = {
    /* The BMS driver's own IOCTLs, function codes 1000-1008. */
    { 0x80180FA0, "IOCTL_BMS_GET_BATTERY_CHARGING_PROFILE", BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_OF(BmsfChargingProfileOut) },
    { 0x80180FA4, "IOCTL_BMS_GET_BATTERY_CURRENT",          BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_OF(BmsfCurrentOut) },
    { 0x80180FA8, "IOCTL_BMS_GET_BATTERY_VOLTAGE",          BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_OF(BmsfVoltageOut) },
    { 0x80180FAC, "IOCTL_BMS_GET_PERCENT_CHARGE",           BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_OF(BmsfPercentChargeOut) },
    { 0x80180FB0, "IOCTL_BMS_SET_CHARGING_STATE",           BMSF_LAYOUT_OF(BmsfChargingStateIn), BMSF_LAYOUT_NONE },
    { 0x80180FB4, "IOCTL_BMS_SET_SYSTEM_INFO",              BMSF_LAYOUT_OF(BmsfSystemInfoIn), BMSF_LAYOUT_OF(BmsfSystemInfoOut) },
    { 0x80180FB8, "IOCTL_BMS_FORCE_OCV",                    BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80180FBC, "IOCTL_BMS_SET_XOADC_CAL_VAL",            BMSF_LAYOUT_OF(BmsfXoadcCalIn), BMSF_LAYOUT_NONE },
    { 0x80180FC0, "IOCTL_BMS_GET_INTERNAL_CALC",            BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_OF(BmsfInternalCalcOut) },

    /*
     * Companion IOCTLs the BMS driver sends to other devices. Not expected on
     * this stack, named only so a stray one is still recognizable.
     */
    { 0x80190FA4, "IOCTL_PM_GAUGE_READ_BMS_OUTPUT_REG_BMS", BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80190FA8, "IOCTL_PM_GAUGE_CALIBRATE_BMS",           BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80190FAC, "IOCTL_PM_GAUGE_ENABLE_BMS",              BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80190FC4, "IOCTL_PM_GAUGE_BMS_OVERRIDE_VBAT_MODE",  BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80190FC8, "IOCTL_PM_GAUGE_BMS_SET_CHARGING_STATE",  BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80190FD0, "IOCTL_PM_GAUGE_BMS_CONFIGURE",           BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021000, "IOCTL_PM_CCADC_READ_DATA",               BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021004, "IOCTL_PM_CCADC_SET_ENABLE",              BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021008, "IOCTL_PM_CCADC_REQUEST_CONVERSION",      BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x8002100C, "IOCTL_PM_CCADC_SET_DECIMATION_RATIO",    BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021010, "IOCTL_PM_CCADC_SET_CONVERSION_RATE",     BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021014, "IOCTL_PM_CCADC_CONNECT_RSENSE",          BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021018, "IOCTL_PM_CCADC_CONFIGURE_OFFSET",        BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x8002101C, "IOCTL_PM_CCADC_CONFIGURE_GAIN",          BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021020, "IOCTL_PM_CCADC_SET_OFFSET_TRIM",         BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021024, "IOCTL_PM_CCADC_GET_CONVERSION_STATUS",   BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x80021030, "IOCTL_PM_CCADC_SET_SEL_SHIFT",           BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
    { 0x800A0FA8, "IOCTL_PM_RTC_GET_TIME",                  BMSF_LAYOUT_UNKNOWN, BMSF_LAYOUT_UNKNOWN },
};

static const BMSF_IOCTL_INFO *
BmsfLookupIoctl(
    _In_ ULONG IoControlCode)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(BmsfIoctlTable); i++) {
        if (BmsfIoctlTable[i].Code == IoControlCode) {
            return &BmsfIoctlTable[i];
        }
    }
    return NULL;
}

/* "IOCTL_BMS_GET_PERCENT_CHARGE", or "UNKNOWN_IOCTL 0x80180FC4" for codes not in the table. */
VOID
BmsfAppendIoctlDescription(
    _Inout_ PBMSF_TEXT Text,
    _In_ ULONG IoControlCode)
{
    const BMSF_IOCTL_INFO *info = BmsfLookupIoctl(IoControlCode);

    if (info != NULL) {
        BmsfTextAppend(Text, info->Name);
    } else {
        BmsfTextAppend(Text, "UNKNOWN_IOCTL ");
        BmsfTextAppendHex(Text, IoControlCode, 8);
    }
}

/*
 * Values of IOCTL_BMS_SET_CHARGING_STATE's input. 0 and 1 are confirmed
 * from device logs: IOCTL_BMS_GET_BATTERY_CURRENT is negative while
 * charging, and 1 is sent when it turns positive (charger unplugged).
 */
static PCSTR
BmsfChargingStateName(
    _In_ LONG State)
{
    switch (State) {
    case 0:  return "charging";
    case 1:  return "discharging";
    case 2:  return "charge-complete / nominal-current rescale";
    default: return "unknown";
    }
}

static ULONG
BmsfReadUlong(
    _In_reads_bytes_(4) const UCHAR *Buffer)
{
    ULONG value;

    /* Byte copy: a mapped MDL buffer is not guaranteed to be 4-byte aligned. */
    RtlCopyMemory(&value, Buffer, sizeof(value));
    return value;
}

static VOID
BmsfAppendFieldValue(
    _Inout_ PBMSF_TEXT Text,
    _In_ BMSF_FIELD_KIND Kind,
    _In_ ULONG Value)
{
    switch (Kind) {
    case BmsfFieldSigned:
        BmsfTextAppendSigned(Text, (LONG)Value);
        break;

    case BmsfFieldHex:
        BmsfTextAppendHex(Text, Value, 8);
        break;

    case BmsfFieldXoadcCode:
        /*
         * PmicBmsSetXoadcCalibration stores ((raw - 0x6000) * 1000 + 5120) / 10240
         * in unsigned 32-bit arithmetic, so codes below 0x6000 wrap around.
         */
        BmsfTextAppendHex(Text, Value, 4);
        BmsfTextAppend(Text, " (stored=");
        BmsfTextAppendUnsigned(Text, ((Value - 0x6000) * 1000 + 5120) / 10240);
        if (Value < 0x6000) {
            BmsfTextAppend(Text, ", wrapped: code below 0x6000");
        }
        BmsfTextAppend(Text, ")");
        break;

    case BmsfFieldChargingState:
        BmsfTextAppendSigned(Text, (LONG)Value);
        BmsfTextAppend(Text, " (");
        BmsfTextAppend(Text, BmsfChargingStateName((LONG)Value));
        BmsfTextAppend(Text, ")");
        break;

    case BmsfFieldUnsigned:
    default:
        BmsfTextAppendUnsigned(Text, Value);
        break;
    }
}

/* "+0x04=0x0000001E (30)" for dwords with no known meaning. */
static VOID
BmsfAppendGenericDwords(
    _Inout_ PBMSF_TEXT Text,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ ULONG ReturnedLength,
    _Inout_ PBOOLEAN First)
{
    ULONG value;
    ULONG count = 0;

    for (; Offset + 4 <= Length; Offset += 4) {
        if (!*First) {
            BmsfTextAppend(Text, ", ");
        }
        *First = FALSE;

        if (count++ == BMSF_GENERIC_DWORD_MAX) {
            BmsfTextAppend(Text, "... (+");
            BmsfTextAppendUnsigned(Text, Length - Offset);
            BmsfTextAppend(Text, " bytes)");
            return;
        }

        value = BmsfReadUlong(Buffer + Offset);
        BmsfTextAppend(Text, "+");
        BmsfTextAppendHex(Text, Offset, 2);
        BmsfTextAppend(Text, "=");
        BmsfTextAppendHex(Text, value, 8);
        BmsfTextAppend(Text, " (");
        BmsfTextAppendSigned(Text, (LONG)value);
        BmsfTextAppend(Text, ")");
        if (Offset + 4 > ReturnedLength) {
            BmsfTextAppend(Text, " [not returned]");
        }
    }

    if (Offset < Length) {
        if (!*First) {
            BmsfTextAppend(Text, ", ");
        }
        *First = FALSE;
        BmsfTextAppend(Text, "tail=");
        BmsfTextAppendRawBytes(Text, Buffer + Offset, Length - Offset, Length - Offset);
    }
}

/*
 * Parses Buffer against Layout. Fields that lie beyond ReturnedLength were
 * written into the buffer but not reported in IoStatus.Information, so with
 * METHOD_BUFFERED they never reach the caller; they are tagged [not returned].
 */
static VOID
BmsfAppendParsedBuffer(
    _Inout_ PBMSF_TEXT Text,
    _In_ const BMSF_LAYOUT *Layout,
    _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_ ULONG ReturnedLength)
{
    BOOLEAN first = TRUE;
    ULONG knownBytes;
    ULONG offset;
    ULONG i;

    if (Length == 0) {
        BmsfTextAppend(Text, "(none)");
        return;
    }
    if (Buffer == NULL) {
        BmsfTextAppend(Text, "(not captured)");
        return;
    }

    if (Layout->Fields == NULL) {
        BmsfTextAppend(Text, "[layout unknown] ");
        BmsfAppendGenericDwords(Text, Buffer, 0, Length, ReturnedLength, &first);
        return;
    }

    for (i = 0, offset = 0; i < Layout->Count && offset + 4 <= Length; i++, offset += 4) {
        if (!first) {
            BmsfTextAppend(Text, ", ");
        }
        first = FALSE;

        BmsfTextAppend(Text, Layout->Fields[i].Name);
        BmsfTextAppend(Text, "=");
        BmsfAppendFieldValue(Text, Layout->Fields[i].Kind, BmsfReadUlong(Buffer + offset));
        if (offset + 4 > ReturnedLength) {
            BmsfTextAppend(Text, " [not returned]");
        }
    }

    knownBytes = Layout->Count * 4;
    if (Length < knownBytes) {
        if (!first) {
            BmsfTextAppend(Text, ", ");
        }
        first = FALSE;
        BmsfTextAppend(Text, "[short buffer: expected ");
        BmsfTextAppendUnsigned(Text, knownBytes);
        BmsfTextAppend(Text, " bytes]");
    }

    /* Whatever follows the known layout, including the tail of a short buffer. */
    if (offset < Length) {
        if (Layout->Count == 0) {
            BmsfTextAppend(Text, "[no data expected] ");
        } else {
            if (!first) {
                BmsfTextAppend(Text, ", ");
            }
            BmsfTextAppend(Text, "extra: ");
        }
        first = TRUE;
        BmsfAppendGenericDwords(Text, Buffer, offset, Length, ReturnedLength, &first);
    }
}

VOID
BmsfAppendInput(
    _Inout_ PBMSF_TEXT Text,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    static const BMSF_LAYOUT unknown = BMSF_LAYOUT_UNKNOWN;
    const BMSF_IOCTL_INFO *info = BmsfLookupIoctl(IoControlCode);

    BmsfAppendParsedBuffer(Text, (info != NULL) ? &info->Input : &unknown, Buffer, Length, Length);
}

VOID
BmsfAppendOutput(
    _Inout_ PBMSF_TEXT Text,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_ ULONG ReturnedLength)
{
    static const BMSF_LAYOUT unknown = BMSF_LAYOUT_UNKNOWN;
    const BMSF_IOCTL_INFO *info = BmsfLookupIoctl(IoControlCode);

    BmsfAppendParsedBuffer(Text, (info != NULL) ? &info->Output : &unknown, Buffer, Length, ReturnedLength);
}

/* "0xC0000010 STATUS_INVALID_DEVICE_REQUEST", or just the hex value. */
VOID
BmsfAppendNtStatus(
    _Inout_ PBMSF_TEXT Text,
    _In_ NTSTATUS Status)
{
    static const struct {
        NTSTATUS Status;
        PCSTR Name;
    } names[] = {
        { STATUS_SUCCESS,                "STATUS_SUCCESS" },
        { STATUS_PENDING,                "STATUS_PENDING" },
        { STATUS_BUFFER_OVERFLOW,        "STATUS_BUFFER_OVERFLOW" },
        { STATUS_UNSUCCESSFUL,           "STATUS_UNSUCCESSFUL" },
        { STATUS_NOT_IMPLEMENTED,        "STATUS_NOT_IMPLEMENTED" },
        { STATUS_INVALID_PARAMETER,      "STATUS_INVALID_PARAMETER" },
        { STATUS_NO_SUCH_DEVICE,         "STATUS_NO_SUCH_DEVICE" },
        { STATUS_INVALID_DEVICE_REQUEST, "STATUS_INVALID_DEVICE_REQUEST" },
        { STATUS_ACCESS_DENIED,          "STATUS_ACCESS_DENIED" },
        { STATUS_BUFFER_TOO_SMALL,       "STATUS_BUFFER_TOO_SMALL" },
        { STATUS_INSUFFICIENT_RESOURCES, "STATUS_INSUFFICIENT_RESOURCES" },
        { STATUS_DEVICE_NOT_READY,       "STATUS_DEVICE_NOT_READY" },
        { STATUS_IO_TIMEOUT,             "STATUS_IO_TIMEOUT" },
        { STATUS_NOT_SUPPORTED,          "STATUS_NOT_SUPPORTED" },
        { STATUS_CANCELLED,              "STATUS_CANCELLED" },
        { STATUS_INVALID_DEVICE_STATE,   "STATUS_INVALID_DEVICE_STATE" },
        { STATUS_DEVICE_DOES_NOT_EXIST,  "STATUS_DEVICE_DOES_NOT_EXIST" },
    };
    ULONG i;

    BmsfTextAppendHex(Text, (ULONG)Status, 8);
    for (i = 0; i < ARRAYSIZE(names); i++) {
        if (names[i].Status == Status) {
            BmsfTextAppend(Text, " ");
            BmsfTextAppend(Text, names[i].Name);
            break;
        }
    }
}
