# bmsIoctlLogger.py

A WinDbg + [pykd](https://github.com/ivellioscolin/pykd) script that traces every IOCTL handled by the Qualcomm PMIC BMS (Battery Management System) driver (`qcbms8930`) on Windows Phone 8.1 (ARM32).

It sets a breakpoint on the driver's IOCTL dispatcher, `PmicBmsIoctlDispatch` (`sub_401090`, RVA `0x1090`). For each call it prints:

- the IOCTL code and its name, or the decoded `CTL_CODE` fields (device type, function, method, access) if the code is unknown
- the parsed input buffer, or a hex dump when the layout isn't known
- the caller (LR) and a call stack, with WDF framework frames filtered out
- on return: the `NTSTATUS`, `BytesReturned` and the parsed output buffer

## Supported IOCTLs

| Code         | Name                                     | Decoded data                              |
|--------------|------------------------------------------|-------------------------------------------|
| `0x80180FA0` | `IOCTL_BMS_GET_BATTERY_CHARGING_PROFILE` | profile/format version (always 4)         |
| `0x80180FA4` | `IOCTL_BMS_GET_BATTERY_CURRENT`          | signed current in mA                      |
| `0x80180FA8` | `IOCTL_BMS_GET_BATTERY_VOLTAGE`          | voltage (~mV)                             |
| `0x80180FAC` | `IOCTL_BMS_GET_PERCENT_CHARGE`           | charge in per-mille (shown as %)          |
| `0x80180FB0` | `IOCTL_BMS_SET_CHARGING_STATE`           | new charging state                        |
| `0x80180FB4` | `IOCTL_BMS_SET_SYSTEM_INFO`              | timestamp, temperature, battery ID        |
| `0x80180FB8` | `IOCTL_BMS_FORCE_OCV`                    | hex dump of the output                    |
| `0x80180FBC` | `IOCTL_BMS_SET_XOADC_CAL_VAL`            | two XOADC raw calibration points          |
| `0x80180FC0` | `IOCTL_BMS_GET_INTERNAL_CALC`            | full-charge capacity, remaining charge values |

## Usage

1. Set `DISPATCH` at the top of the script to the dispatcher's address in your session. The default is `"qcbms8930+0x1090"`. You can also use a `module!symbol` name or an absolute address.
2. From the kernel debugger:

   ```
   kd> .load pykd
   kd> !py
   >>> import runpy
   >>> runpy.run_path(r"C:\path\to\bmsIoctlLogger.py")
   >>> quit()
   kd> g
   ```

The log is printed in the debugger console while the target runs.

## Notes

- **Driver not loaded yet:** if `ARM_ON_LOAD = True` (the default), the script sets a module-load break (`sxe ld`) and a load handler, so the breakpoint is placed as soon as the driver loads.
- **Running it again:** the breakpoints from the previous run are removed first, so you can edit the script and re-run it in the same session.
- **Breakpoints stay active:** the script stores its namespace on `__main__`, so its breakpoint callbacks keep working after `runpy.run_path()` returns.
- **Hex dump size:** `MAX_DUMP` sets the maximum number of bytes shown in raw hex dumps (default 64).
