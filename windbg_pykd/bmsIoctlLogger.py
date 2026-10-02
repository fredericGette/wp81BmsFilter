# -----------------------------------------------------------------------------
# bms_ioctl_logger.py
#
# WinDbg + pykd script that logs every IOCTL processed by the PMIC BMS WDF
# driver's PmicBmsIoctlDispatch (sub_401090).
#
# For each call it prints:
#   - the IOCTL code + a human-readable description (decoded fields for unknowns)
#   - the parsed input buffer
#   - the parsed output buffer + NTSTATUS, captured when the function returns
#
# TARGET: ARM32 (WP8.1 / Qualcomm MSM). AAPCS calling convention:
#   r0..r3 = first four args, remaining args on the stack at [sp], [sp+4], ...
#
# PmicBmsIoctlDispatch(
#     PPMIC_BMS_DEVICE_CONTEXT ctx,   // r0
#     ULONG IoControlCode,            // r1
#     PULONG InputBuffer,             // r2
#     size_t InputBufferLength,       // r3
#     PULONG OutputBuffer,            // [sp+0x00]
#     size_t OutputBufferLength,      // [sp+0x04]
#     ULONG_PTR *BytesReturned);      // [sp+0x08]
#
# USAGE (load via runpy from the interactive pykd console):
#     kd> .load pykd
#     kd> !py
#     >>> import runpy
#     >>> runpy.run_path(r"C:\users\frede\downloads\bms_ioctl_logger.py")
#     >>> quit()          # leave the Python console, back to kd>
#     kd> g               # run the driver; the log streams here
#
# Because runpy.run_path() runs in a throwaway namespace, this script pins its
# own namespace onto the persistent interactive __main__ so the breakpoints and
# their Python callbacks stay alive after run_path() returns. Re-running the
# script removes the breakpoints from the previous run first.
#
# >>> EDIT 'DISPATCH' BELOW to point at PmicBmsIoctlDispatch in your session. <<<
# -----------------------------------------------------------------------------

import re
import pykd

# =============================== CONFIGURATION ===============================
# Address of PmicBmsIoctlDispatch (sub_401090).
# sub_401090 implies an image base of 0x400000, i.e. RVA 0x1090.
# Pick whichever form matches your session:
#   - a MASM expression using the loaded module name : "QCPMICBMS+0x1090"
#   - "module!symbol" if you happen to have symbols   : "QCPMICBMS!PmicBmsIoctlDispatch"
#   - an absolute hex address                         : "0xFFFFF80012341090"
DISPATCH = "qcbms8930+0x1090"

# Cap for the raw hex dump of unknown / raw buffers.
MAX_DUMP = 64

# Key under which this script pins its namespace on the interactive __main__.
NS_KEY = "_bms_ioctl_logger_ns"

# If the target module is not loaded yet when the script runs, arm a module
# load break (sxe ld) + an auto-arm handler so the entry breakpoint installs
# itself the moment the driver loads. Set False to just report and stop.
ARM_ON_LOAD = True
# ============================================================================


# --- pykd eventResult "continue execution" value (portable across versions) ---
def _resolve_proceed():
    er = getattr(pykd, "eventResult", None)
    if er is not None:
        for n in ("Proceed", "proceed", "Go", "go"):
            if hasattr(er, n):
                return getattr(er, n)
    return None

PROCEED = _resolve_proceed()


def _resolve_eventresult(*names):
    er = getattr(pykd, "eventResult", None)
    if er is not None:
        for n in names:
            if hasattr(er, n):
                return getattr(er, n)
    return None

BREAK = _resolve_eventresult("Break", "break")
NOCHANGE = _resolve_eventresult("NoChange", "noChange", "no_change")


# --------------------------------- helpers ----------------------------------
def resolve_addr(expr):
    """Resolve a config string to an address: int, module!symbol, or MASM expr."""
    if isinstance(expr, int):
        return expr
    try:
        return int(expr, 0)
    except (TypeError, ValueError):
        pass
    try:
        return pykd.getOffset(expr)          # module!symbol
    except Exception:
        pass
    return pykd.expr(expr)                    # MASM expression ("mod+0x1090")


# findSymbol on a 32-bit target may render "module+offset" with the offset
# sign-extended to 64 bits (e.g. "qcbms8930+100001756"). Mask the trailing
# offset to 32 bits and normalize to "module+0xRVA". Symbols without a
# trailing hex offset (bare "0x........", "module!func") are left unchanged.
_SYM_OFF_RE = re.compile(r'^(.*\+)(?:0x)?([0-9A-Fa-f]+)$')


def mask_sym(sym):
    if not isinstance(sym, str):
        return sym
    m = _SYM_OFF_RE.match(sym)
    if not m:
        return sym
    try:
        off = int(m.group(2), 16) & 0xFFFFFFFF
    except ValueError:
        return sym
    return "%s0x%X" % (m.group(1), off)


def reg(name):
    return pykd.reg(name) & 0xFFFFFFFF


def reg_sp():
    try:
        return pykd.reg("sp") & 0xFFFFFFFF
    except Exception:
        return pykd.reg("r13") & 0xFFFFFFFF


def reg_lr():
    try:
        return pykd.reg("lr") & 0xFFFFFFFF
    except Exception:
        return pykd.reg("r14") & 0xFFFFFFFF


def rd_u32(addr):
    return pykd.ptrDWord(addr) & 0xFFFFFFFF


def rd_s32(addr):
    return pykd.ptrSignDWord(addr)


def s32(x):
    x &= 0xFFFFFFFF
    return x - 0x100000000 if (x & 0x80000000) else x


def in_dwords(addr, length, count):
    vals = []
    for i in range(count):
        if length is not None and (i + 1) * 4 > length:
            break
        vals.append(rd_u32(addr + 4 * i))
    return vals


def hexdump_lines(addr, length, maxlen=MAX_DUMP):
    out = []
    if addr == 0:
        return ["      (buffer is NULL)"]
    n = length if length is not None else 0
    n = min(n, maxlen)
    if n <= 0:
        return ["      (empty)"]
    try:
        data = pykd.loadBytes(addr, n)
    except Exception as e:
        return ["      <unreadable @ 0x%08X: %s>" % (addr, e)]
    for off in range(0, n, 16):
        chunk = data[off:off + 16]
        hexs = " ".join("%02X" % (b & 0xFF) for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("      +%04X  %-47s  %s" % (off, hexs, asc))
    if length and length > maxlen:
        out.append("      ... (%d more bytes not shown)" % (length - maxlen))
    return out


# XOADC raw code -> converted value, mirroring the driver's rounding:
#   ((raw - 0x6000) * 1000 + 5120) / 10240   (unsigned; ~0.0977 units/LSB)
def xoadc_conv(raw):
    diff = (raw - 0x6000) & 0xFFFFFFFF
    return ((diff * 1000 + 5120) // 10240) & 0xFFFFFFFF


def charging_state_name(st):
    return {
        0: "not charging / idle",
        1: "charging",
        2: "charge-complete / nominal-current rescale",
    }.get(st, "unknown")


# ----------------------------- IOCTL metadata -------------------------------
IOCTL_NAMES = {
    0x80180FA0: "IOCTL_BMS_GET_BATTERY_CHARGING_PROFILE",
    0x80180FA4: "IOCTL_BMS_GET_BATTERY_CURRENT",
    0x80180FA8: "IOCTL_BMS_GET_BATTERY_VOLTAGE",
    0x80180FAC: "IOCTL_BMS_GET_PERCENT_CHARGE",
    0x80180FB0: "IOCTL_BMS_SET_CHARGING_STATE",
    0x80180FB4: "IOCTL_BMS_SET_SYSTEM_INFO",
    0x80180FB8: "IOCTL_BMS_FORCE_OCV",
    0x80180FBC: "IOCTL_BMS_SET_XOADC_CAL_VAL",
    0x80180FC0: "IOCTL_BMS_GET_INTERNAL_CALC",
}


def decode_ioctl(code):
    devtype = (code >> 16) & 0xFFFF
    access = (code >> 14) & 0x3
    func = (code >> 2) & 0xFFF
    method = code & 0x3
    methods = {0: "BUFFERED", 1: "IN_DIRECT", 2: "OUT_DIRECT", 3: "NEITHER"}
    accs = {0: "ANY", 1: "READ", 2: "WRITE", 3: "READ|WRITE"}
    return ("devtype=0x%04X func=%d(0x%X) method=%s access=%s"
            % (devtype, func, func, methods.get(method, "?"), accs.get(access, "?")))


def ioctl_name(code):
    return IOCTL_NAMES.get(code, "UNKNOWN_IOCTL")


# ------------------------------- input parser -------------------------------
def format_input(ioctl, addr, length):
    lines = []
    if addr == 0 or not length:
        lines.append("      (no input data)")
        return lines

    if ioctl == 0x80180FB0:  # SET_CHARGING_STATE
        vals = in_dwords(addr, length, 1)
        if vals:
            st = vals[0]
            lines.append("      NewChargingState = %d (%s)"
                         % (st, charging_state_name(st)))
        else:
            lines.extend(hexdump_lines(addr, length))

    elif ioctl == 0x80180FB4:  # SET_SYSTEM_INFO
        v = in_dwords(addr, length, 3)
        if len(v) >= 3:
            lines.append("      Timestamp   = 0x%08X (%u)" % (v[0], v[0]))
            lines.append("      Temperature = 0x%08X (%d)   [-> temp index dword_419150]"
                         % (v[1], s32(v[1])))
            lines.append("      BatteryId   = 0x%08X (%u)" % (v[2], v[2]))
        else:
            lines.extend(hexdump_lines(addr, length))

    elif ioctl == 0x80180FBC:  # SET_XOADC_CAL_VAL
        v = in_dwords(addr, length, 2)
        if len(v) >= 2:
            lines.append("      RawPointA = 0x%08X (%u)  ~conv %u"
                         % (v[0], v[0], xoadc_conv(v[0])))
            lines.append("      RawPointB = 0x%08X (%u)  ~conv %u"
                         % (v[1], v[1], xoadc_conv(v[1])))
        else:
            lines.extend(hexdump_lines(addr, length))

    else:
        # GET_* commands and FORCE_OCV: no documented input structure -> raw dump
        lines.extend(hexdump_lines(addr, length))

    return lines


# ------------------------------- output parser ------------------------------
def format_output(ioctl, addr, length, bytes_returned):
    lines = []

    if ioctl in (0x80180FB0, 0x80180FBC):
        # SET_CHARGING_STATE writes no output; SET_XOADC_CAL_VAL reports 4 but
        # the backend produces no real output data.
        lines.append("      (no meaningful output data)")
        if ioctl == 0x80180FBC:
            lines.append("      note: BytesReturned reads 4 but backend writes nothing")
        return lines

    if addr == 0:
        lines.append("      (output buffer is NULL)")
        return lines

    try:
        if ioctl == 0x80180FA0:      # GET_BATTERY_CHARGING_PROFILE (const 4)
            v = rd_u32(addr)
            lines.append("      Result = %u  (profile/format version, expected const 4)" % v)

        elif ioctl == 0x80180FA4:    # GET_BATTERY_CURRENT (signed mA)
            v = rd_s32(addr)
            lines.append("      BatteryCurrent = %d mA" % v)

        elif ioctl == 0x80180FA8:    # GET_BATTERY_VOLTAGE (selector-6 scale ~ mV)
            v = rd_u32(addr)
            lines.append("      BatteryVoltage = %u   (~mV, selector-6 scale)" % v)

        elif ioctl == 0x80180FAC:    # GET_PERCENT_CHARGE (per-mille)
            v = rd_u32(addr)
            lines.append("      PercentCharge = %u   (per-mille -> %.1f %%)" % (v, v / 10.0))

        elif ioctl == 0x80180FC0:    # GET_INTERNAL_CALC (12 bytes: 3 dwords)
            d0 = rd_u32(addr)
            d1 = rd_u32(addr + 4)
            d2 = rd_u32(addr + 8)
            lines.append("      DeratedFullChargeCapacity = %u   (dword_42C8D8)" % d0)
            lines.append("      RemainingChargeReference  = %u   (dword_42C8D4)" % d1)
            lines.append("      RemainingChargeHeadroom   = %u   (dword_42C91C)" % d2)
            lines.append("      note: BytesReturned reads 4 but 12 bytes are written")

        elif ioctl == 0x80180FB4:    # SET_SYSTEM_INFO -> ResultOut (LONG)
            v = rd_s32(addr)
            lines.append("      Result = %d" % v)

        elif ioctl == 0x80180FB8:    # FORCE_OCV: output layout not documented
            n = length if length else 8
            lines.extend(hexdump_lines(addr, n))

        else:                        # unknown code
            lines.extend(hexdump_lines(addr, length))
    except Exception as e:
        lines.append("      <output read error @ 0x%08X: %s>" % (addr, e))

    return lines


# ------------------------------ breakpoint state ----------------------------
# All mutable state lives in this dict so it can be reached from the persisted
# namespace for cleanup on re-run.
STATE = {
    "seq": 0,
    "pending": [],       # LIFO of in-flight calls
    "bps": [],           # breakpoint objects created by this run
    "exit_bp": None,     # the return breakpoint, armed lazily on first entry
    "load_waiter": None, # module-load event handler, if armed
}


def _remove_bp(bp):
    for attempt in (lambda: pykd.removeBp(bp), lambda: bp.remove()):
        try:
            attempt()
            return
        except Exception:
            continue


# Stack frames from these modules are framework plumbing (the WDF layer
# between the I/O manager and the driver) and are filtered out of the
# printed stack. Case-insensitive substring match against the symbol.
STACK_SKIP_MODULES = ("Wdf01000",)


def _stack_lines(max_frames=12):
    """Best-effort call stack for the current thread, with the modules in
    STACK_SKIP_MODULES filtered out and the remaining frames renumbered.

    At the entry breakpoint the prologue (push {..,lr}) has not run, so the
    frame is not yet established and an unwinder may drop the immediate caller;
    LR is printed separately by the caller as the guaranteed-correct one."""
    out = []
    try:
        stk = pykd.getStack()
    except Exception as e:
        return ["      <stack unavailable: %s>" % e]
    idx = 0
    for fr in stk:
        ip = getattr(fr, "ip", None)
        if ip is None:
            ip = getattr(fr, "instructionOffset", 0)
        ip &= 0xFFFFFFFF
        try:
            sym = mask_sym(pykd.findSymbol(ip))
        except Exception:
            sym = "0x%08X" % ip
        if any(m.lower() in sym.lower() for m in STACK_SKIP_MODULES):
            continue
        out.append("      %02d  0x%08X  %s" % (idx, ip, sym))
        idx += 1
        if idx >= max_frames:
            out.append("      ...")
            break
    if not out:
        out.append("      <empty>")
    return out


def on_entry(bpId=None):
    try:
        ioctl = reg("r1")
        in_buf = reg("r2")
        in_len = reg("r3")
        sp = reg_sp()
        out_buf = rd_u32(sp + 0x00)
        out_len = rd_u32(sp + 0x04)
        br_ptr = rd_u32(sp + 0x08)
        lr = reg_lr()

        STATE["seq"] += 1
        seq = STATE["seq"]

        print("")
        print("==================== BMS IOCTL #%d ====================" % seq)
        print("  IoControlCode : 0x%08X  %s" % (ioctl, ioctl_name(ioctl)))
        print("  InputBuffer   : 0x%08X  len=%u (0x%X)" % (in_buf, in_len, in_len))
        print("  OutputBuffer  : 0x%08X  len=%u (0x%X)" % (out_buf, out_len, out_len))
        print("  --- input ---")
        for ln in format_input(ioctl, in_buf, in_len):
            print(ln)

        # Who issued this IOCTL: immediate caller (LR), then a best-effort
        # stack walk (framework plumbing filtered out).
        try:
            lr_sym = mask_sym(pykd.findSymbol(lr & ~1))
        except Exception:
            lr_sym = "0x%08X" % (lr & ~1)
        print("  --- caller ---")
        print("  return (LR)   : 0x%08X  %s" % (lr & ~1, lr_sym))
        print("  --- stack ---")
        for ln in _stack_lines():
            print(ln)

        # Arm the return breakpoint once (LR is constant: the single call site
        # in EvtWdfIoQueueIoDeviceControl). Clear the Thumb bit (bit0) of LR.
        ret = lr & ~1
        if STATE["exit_bp"] is None:
            STATE["exit_bp"] = pykd.setBp(ret, on_exit)
            STATE["bps"].append(STATE["exit_bp"])
            print("  (return breakpoint armed at 0x%08X)" % ret)

        STATE["pending"].append({
            "seq": seq,
            "ioctl": ioctl,
            "out_buf": out_buf,
            "out_len": out_len,
            "br_ptr": br_ptr,
        })
    except Exception as e:
        print("[bms-ioctl] entry handler error: %s" % e)

    return PROCEED


def on_exit(bpId=None):
    try:
        pend = STATE["pending"]
        if not pend:
            # Return address reached without a tracked entry; ignore.
            return PROCEED

        f = pend.pop()
        status = reg("r0")

        bytes_returned = None
        if f["br_ptr"]:
            try:
                bytes_returned = rd_u32(f["br_ptr"])
            except Exception:
                bytes_returned = None

        print("  --- output (return) ---")
        print("  NTSTATUS      : 0x%08X" % status)
        if bytes_returned is not None:
            print("  BytesReturned : %u (0x%X)" % (bytes_returned, bytes_returned))
        for ln in format_output(f["ioctl"], f["out_buf"], f["out_len"], bytes_returned):
            print(ln)
        print("======================================================")
    except Exception as e:
        print("[bms-ioctl] exit handler error: %s" % e)

    return PROCEED


# --------------------------------- install ----------------------------------
def _module_name(expr):
    """Extract the module name from a DISPATCH expression, or None for a bare
    address / symbol with no module to wait on."""
    if not isinstance(expr, str):
        return None
    for sep in ("+", "!"):
        if sep in expr:
            return expr.split(sep, 1)[0].strip()
    return None


def _module_loaded(name):
    """True if a module by this name is currently loaded (or we can't tell)."""
    if not name:
        return True   # absolute address: let resolve_addr try directly
    try:
        pykd.module(name)
        return True
    except Exception:
        return False


def _remove_load_waiter(waiter):
    if waiter is None:
        return
    for attempt in (lambda: waiter.removeHandler(),
                    lambda: waiter.remove(),
                    lambda: pykd.removeEventHandler(waiter)):
        try:
            attempt()
            return
        except Exception:
            continue


def _make_load_waiter():
    """Build a pykd module-load event handler class, or None if this pykd
    build has no eventHandler base (defined lazily so import never fails)."""
    if not hasattr(pykd, "eventHandler"):
        return None

    class _LoadWaiter(pykd.eventHandler):
        def __init__(self, module_name, dispatch_expr):
            pykd.eventHandler.__init__(self)
            self.module_name = module_name.lower()
            self.dispatch_expr = dispatch_expr
            self.armed = False

        def onLoadModule(self, *args):
            # args is (base,) or (moduleObject,) depending on the pykd build.
            try:
                name = None
                if args:
                    a = args[0]
                    if isinstance(a, int):
                        try:
                            name = pykd.module(a).name()
                        except Exception:
                            name = None
                    else:
                        try:
                            name = a.name()
                        except Exception:
                            name = None
                if (name and name.lower() == self.module_name
                        and not self.armed):
                    addr = resolve_addr(self.dispatch_expr) & 0xFFFFFFFF
                    bp = pykd.setBp(addr, on_entry)
                    STATE["bps"].append(bp)
                    self.armed = True
                    print("")
                    print("[bms-ioctl] '%s' loaded; entry breakpoint armed @ 0x%08X"
                          % (self.module_name, addr))
                    print("[bms-ioctl] type 'g' to continue logging.")
                    return BREAK if BREAK is not None else NOCHANGE
            except Exception as e:
                print("[bms-ioctl] onLoadModule error: %s" % e)
            return NOCHANGE

    return _LoadWaiter


def _cleanup_previous():
    """Remove breakpoints and the load handler left by a previous run."""
    import __main__
    prev = getattr(__main__, NS_KEY, None)
    if not prev:
        return
    old_state = prev.get("STATE") if isinstance(prev, dict) else None
    if old_state:
        for bp in old_state.get("bps", []):
            _remove_bp(bp)
        _remove_load_waiter(old_state.get("load_waiter"))
        old_state["bps"] = []
        old_state["exit_bp"] = None
        old_state["pending"] = []
        old_state["load_waiter"] = None
    try:
        delattr(__main__, NS_KEY)
    except Exception:
        pass


def _pin_namespace():
    """Keep this module's namespace (and thus the callbacks) alive for the
    whole debugging session, since runpy.run_path drops it on return."""
    import __main__
    setattr(__main__, NS_KEY, globals())


def _install_now(addr):
    addr &= 0xFFFFFFFF
    entry_bp = pykd.setBp(addr, on_entry)
    STATE["bps"].append(entry_bp)
    print("[bms-ioctl] logging PmicBmsIoctlDispatch @ 0x%08X" % addr)
    print("[bms-ioctl] entry breakpoint set.")
    print("[bms-ioctl] leave this console (quit()) and type 'g' at kd> to run.")
    # Clear any stale module-load break from a previous, not-yet-loaded run.
    module = _module_name(DISPATCH)
    if module:
        try:
            pykd.dbgCommand("sxd ld:%s" % module)
        except Exception:
            pass


def _arm_on_load(module):
    """Module not loaded yet: register an auto-arm handler and a native load
    break so the debugger stops (and the entry bp installs) at load time."""
    print("[bms-ioctl] module '%s' is not loaded yet." % module)

    cls = _make_load_waiter()
    if cls is not None:
        try:
            STATE["load_waiter"] = cls(module, DISPATCH)
            print("[bms-ioctl] load handler registered; entry bp will self-arm on load.")
        except Exception as e:
            print("[bms-ioctl] could not register load handler: %s" % e)
    else:
        print("[bms-ioctl] (this pykd has no eventHandler; relying on 'sxe ld').")

    try:
        pykd.dbgCommand("sxe ld:%s" % module)
        print("[bms-ioctl] 'sxe ld:%s' set: debugger will stop when it loads." % module)
    except Exception as e:
        print("[bms-ioctl] could not set load break: %s" % e)

    print("[bms-ioctl] leave this console (quit()) and type 'g'. It will stop at")
    print("            load; if the entry bp did not self-arm, just re-run this script.")


def main():
    _cleanup_previous()
    _pin_namespace()

    module = _module_name(DISPATCH)

    if _module_loaded(module):
        try:
            addr = resolve_addr(DISPATCH)
        except Exception as e:
            print("[bms-ioctl] could not resolve DISPATCH=%r : %s" % (DISPATCH, e))
            print("            Edit the DISPATCH constant at the top of the script.")
            return
        _install_now(addr)
    elif ARM_ON_LOAD:
        _arm_on_load(module)
    else:
        print("[bms-ioctl] module '%s' not loaded and ARM_ON_LOAD is False; nothing set."
              % module)

    if PROCEED is None:
        print("[bms-ioctl] WARNING: could not find pykd.eventResult.Proceed; the")
        print("            debugger may break on each IOCTL instead of auto-continuing.")


main()