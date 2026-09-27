# wp81BmsFilter

KMDF 1.11 upper filter for the Qualcomm PMIC BMS driver on Windows Phone 8.1
(device type `0x8018`, symbolic link `\\.\QCOMPMICBMS`). It logs every
`IRP_MJ_DEVICE_CONTROL` sent to the BMS device to ETW and passes the request
on unchanged. All other request types go straight through.

## Build

Needs LLVM (clang-cl, lld-link), CMake and Ninja, plus the kits at their default locations:

- `C:\Program Files (x86)\Windows Kits\8.1`: KMDF 1.11 headers, ARM kernel and KMDF libs
- `C:\Program Files (x86)\Windows Phone Kits\8.1`: `wdm.h` and the shared headers

```
cmake --preset arm32-kmdf
cmake --build build
```

Output: `build\wp81BmsFilter.sys` (ARM Thumb-2, native subsystem 6.3, imports only
`ntoskrnl.exe` and `WDFLDR.SYS`) and `build\wp81BmsFilter.pdb`.

## Install (on the phone)

1. Copy `wp81BmsFilter.sys` to `C:\Windows\System32\drivers\`.
2. Create the service:
   ```
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81BmsFilter /v Type /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81BmsFilter /v Start /t REG_DWORD /d 3
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81BmsFilter /v ErrorControl /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81BmsFilter /v ImagePath /t REG_EXPAND_SZ /d \SystemRoot\System32\drivers\wp81BmsFilter.sys
   ```
3. Add `wp81BmsFilter` to the `UpperFilters` value (`REG_MULTI_SZ`) of the BMS
   device instance.
  ```
  reg ADD "HKEY_LOCAL_MACHINE\System\CurrentControlSet\Enum\ACPI\QCOM0A02\2&daba3ff&0" /V UpperFilters /T REG_MULTI_SZ /D "wp81BmsFilter"
  ```
4. Reboot. The BMS driver disables PnP stop/remove, so the stack is only rebuilt at boot.

The driver must be signed in a way the device accepts (or test signing enabled).

## ETW provider

| | |
|---|---|
| Name | `WP81-BmsFilter` |
| GUID | `{965A5080-7C4B-4E1A-9E3E-52FEFE214A5C}` |
| Keyword `0x1` | request received (Information) |
| Keyword `0x2` | request completed (Information, or Warning if the status is an error) |
| Keyword `0x4` | filter lifecycle and internal failures |

Events are plain strings (`EtwWriteString`), so no manifest has to be
registered to read them. Capture with any ETW controller, for example [wp81debug](https://github.com/fredericGette/wp81debug):

```
wp81debug.exe etw {965A5080-7C4B-4E1A-9E3E-52FEFE214A5C}
```


## Log format

Each IOCTL produces a `REQ` event when it arrives and a `CPL` event when the
BMS driver completes it. Both carry the same `#` number:

```
REQ #7 IOCTL_BMS_SET_SYSTEM_INFO in=12 out=4 | Input: Timestamp?=1700000000, Temperature?=-5, BatteryId?=4660 | InputRaw: 00 F1 53 65 FB FF FF FF 34 12 00 00
CPL #7 IOCTL_BMS_SET_SYSTEM_INFO status=0x00000000 STATUS_SUCCESS returned=4/4 | Output: Result?=0 | OutputRaw: 00 00 00 00
REQ #8 IOCTL_BMS_SET_CHARGING_STATE in=4 out=0 | Input: NewChargingState=1 (discharging) | InputRaw: 01 00 00 00
```

- Codes not in the table are logged as `UNKNOWN_IOCTL 0x<code>` and their
  buffers are shown as generic dwords.
- `IOCTL_BMS_SET_CHARGING_STATE` shows the meaning of its value: `0`
  charging, `1` discharging, `2` charge-complete / nominal-current rescale.
  `IOCTL_BMS_GET_BATTERY_CURRENT` is negative while charging and positive while
  discharging.
- Field names ending in `?` are layouts inferred from the reverse-engineered
  backend signatures, not yet confirmed. The table is in `src/BmsIoctl.c`.
- The input is logged before the request is forwarded. With `METHOD_BUFFERED`
  the BMS driver writes its output over the same buffer.
- On success the whole output buffer is parsed. Bytes past `IoStatus.Information`
  are marked `[not returned]` (and after `|` in the raw dump): the BMS driver
  writes them, but they never reach the caller.
- On an error status the output is not parsed; only the reported bytes are dumped.
- `METHOD_NEITHER` buffers are not captured (user addresses, caller context only).
