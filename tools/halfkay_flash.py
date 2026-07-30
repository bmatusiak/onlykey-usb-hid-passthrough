"""Flash a Teensy HalfKay bootloader over raw HID, on Windows, with no toolchain.

A Python port of the parts of teensy_loader_cli we need. It exists because
teensy_loader_cli has to be compiled and there is no native compiler on this
machine -- and because having the flasher scriptable makes it possible to
exercise the USB proxy repeatedly and instrument what it does.

Writes go out through HidD_SetOutputReport, i.e. a control-pipe SET_REPORT,
which is the same path the proxy forwards. For a Teensy 3.2 each block is a
1089-byte report (1 report-ID byte + 3 address bytes + 61 pad + 1024 data).

    python halfkay_flash.py firmware.hex              # program then reboot
    python halfkay_flash.py --boot-only               # just reboot the key
    python halfkay_flash.py firmware.hex --no-reboot  # leave it in bootloader

Defaults target the OnlyKey's MK20DX256 (Teensy 3.2). Override with --code-size
and --block-size for other parts.
"""

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

# ---------------------------------------------------------------- Win32 ----

hid = ctypes.WinDLL("hid")
setupapi = ctypes.WinDLL("setupapi")
kernel32 = ctypes.WinDLL("kernel32")

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
INVALID_HANDLE = ctypes.c_void_p(-1).value

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10


class GUID(ctypes.Structure):
    _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD),
                ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", ctypes.POINTER(ctypes.c_ulong))]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
                ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


# Explicit signatures are not optional here: without them ctypes truncates
# 64-bit HANDLEs and the calls fail with confusing overflow errors.
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR,
                                          wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p,
    wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]

hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_SetOutputReport.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG]


def find_device(vid, pid):
    """Return (handle, output_report_len) for the first matching HID device."""
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    dev_info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if dev_info == INVALID_HANDLE:
        raise OSError("SetupDiGetClassDevs failed")

    try:
        idx = 0
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(
                    dev_info, None, ctypes.byref(guid), idx, ctypes.byref(iface)):
                return None, 0
            idx += 1

            needed = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                dev_info, ctypes.byref(iface), None, 0, ctypes.byref(needed), None)
            buf = ctypes.create_string_buffer(needed.value)
            # cbSize of the detail struct itself: 8 on 64-bit, 6 on 32-bit.
            ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = (
                8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6)
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                    dev_info, ctypes.byref(iface), buf, needed, None, None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(buf) + ctypes.sizeof(wintypes.DWORD))

            handle = kernel32.CreateFileW(
                path, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
            if handle == INVALID_HANDLE:
                continue

            attrs = HIDD_ATTRIBUTES()
            attrs.Size = ctypes.sizeof(attrs)
            if hid.HidD_GetAttributes(handle, ctypes.byref(attrs)) and \
                    attrs.VendorID == vid and attrs.ProductID == pid:
                pp = ctypes.c_void_p()
                out_len = 0
                if hid.HidD_GetPreparsedData(handle, ctypes.byref(pp)):
                    caps = HIDP_CAPS()
                    hid.HidP_GetCaps(pp, ctypes.byref(caps))
                    out_len = caps.OutputReportByteLength
                    hid.HidD_FreePreparsedData(pp)
                    print("found %04X:%04X  usage_page=0x%04X usage=0x%04X  "
                          "output report=%d bytes"
                          % (vid, pid, caps.UsagePage, caps.Usage, out_len))
                return handle, out_len

            kernel32.CloseHandle(handle)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(dev_info)


# ------------------------------------------------------------ Intel HEX ----

def read_intel_hex(path, code_size):
    """Return (memory bytearray, used bytearray) for a flat image."""
    mem = bytearray(b"\xff" * code_size)
    used = bytearray(code_size)
    base = 0
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":"):
                continue
            raw = bytes.fromhex(line[1:])
            count, addr_hi, addr_lo, rtype = raw[0], raw[1], raw[2], raw[3]
            addr = (addr_hi << 8) | addr_lo
            data = raw[4:4 + count]
            if rtype == 0x00:
                for i, byte in enumerate(data):
                    off = base + addr + i
                    if 0 <= off < code_size:
                        mem[off] = byte
                        used[off] = 1
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                base = ((data[0] << 8) | data[1]) * 16
            elif rtype == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
    return mem, used


# ---------------------------------------------------------------- flash ----

def write_report(handle, payload, out_len):
    """Send one HalfKay packet as a control-pipe SET_REPORT (report ID 0)."""
    buf = ctypes.create_string_buffer(out_len)
    buf[0] = 0  # report ID
    ctypes.memmove(ctypes.addressof(buf) + 1, bytes(payload), len(payload))
    return bool(hid.HidD_SetOutputReport(handle, buf, out_len))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hexfile", nargs="?")
    ap.add_argument("--vid", type=lambda s: int(s, 16), default=0x16C0)
    ap.add_argument("--pid", type=lambda s: int(s, 16), default=0x0478)
    ap.add_argument("--code-size", type=int, default=262144)   # MK20DX256
    ap.add_argument("--block-size", type=int, default=1024)
    ap.add_argument("--boot-only", action="store_true")
    ap.add_argument("--no-reboot", action="store_true")
    args = ap.parse_args()

    if not args.hexfile and not args.boot_only:
        ap.error("need a .hex file (or --boot-only)")

    handle, out_len = find_device(args.vid, args.pid)
    if not handle:
        print("no %04X:%04X device found -- is it in bootloader mode?"
              % (args.vid, args.pid))
        return 1

    # 3 address bytes + 61 pad + block, matching teensy_loader_cli for
    # block_size 512/1024.
    write_size = args.block_size + 64
    if out_len and out_len != write_size + 1:
        print("WARNING: device reports %d-byte output reports, expected %d"
              % (out_len, write_size + 1))
    if not out_len:
        out_len = write_size + 1

    try:
        if args.boot_only:
            payload = bytearray(write_size)
            payload[0] = payload[1] = payload[2] = 0xFF
            print("rebooting..." if write_report(handle, payload, out_len)
                  else "reboot write FAILED")
            return 0

        mem, used = read_intel_hex(args.hexfile, args.code_size)
        total = sum(used)
        print("read %s: %d bytes (%.1f%% of %d)"
              % (args.hexfile, total, 100.0 * total / args.code_size, args.code_size))

        started = time.time()
        blocks = 0
        for addr in range(0, args.code_size, args.block_size):
            chunk = used[addr:addr + args.block_size]
            # Always send block 0 -- it triggers the chip erase.
            if blocks > 0 and not any(chunk):
                continue

            payload = bytearray(write_size)
            payload[0] = addr & 0xFF
            payload[1] = (addr >> 8) & 0xFF
            payload[2] = (addr >> 16) & 0xFF
            payload[64:64 + args.block_size] = mem[addr:addr + args.block_size]

            if not write_report(handle, payload, out_len):
                err = ctypes.get_last_error()
                print("\nFAILED writing block at 0x%06X (block %d), win32 error %d"
                      % (addr, blocks, err))
                return 1

            # The first write erases the chip and takes far longer than the rest.
            if blocks == 0:
                time.sleep(3.0)
            blocks += 1
            if blocks % 32 == 0:
                sys.stdout.write(".")
                sys.stdout.flush()

        elapsed = time.time() - started
        print("\nprogrammed %d blocks (%d bytes) in %.2f s"
              % (blocks, blocks * args.block_size, elapsed))

        if not args.no_reboot:
            payload = bytearray(write_size)
            payload[0] = payload[1] = payload[2] = 0xFF
            write_report(handle, payload, out_len)
            print("reboot sent")
        return 0
    finally:
        kernel32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())
