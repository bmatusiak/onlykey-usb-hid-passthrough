"""Minimal Windows HID access: find a device by VID/PID/interface, write, read.

Reads use OVERLAPPED I/O with a real timeout. A plain blocking ReadFile on a
HID device that never sends anything waits forever, and a test harness that can
hang is worse than no harness -- the whole point of this rig is that failures
are immediate and located.

`halfkay_flash.py` keeps its own copy of the device-finding code. That is
deliberate: it is proven against the bootloader and flashing must not break, so
it is not being refactored underneath a working flasher. This module is for
everything else.
"""
import ctypes
import time
from ctypes import wintypes

hid = ctypes.WinDLL("hid")
setupapi = ctypes.WinDLL("setupapi")
kernel32 = ctypes.WinDLL("kernel32")

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE = ctypes.c_void_p(-1).value
WAIT_TIMEOUT = 0x102
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10


class GUID(ctypes.Structure):
    _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD),
                ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD),
                ("Reserved", ctypes.POINTER(ctypes.c_ulong))]


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


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.POINTER(ctypes.c_ulong)),
                ("InternalHigh", ctypes.POINTER(ctypes.c_ulong)),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]


# Without explicit signatures ctypes truncates 64-bit HANDLEs and the calls fail
# with confusing overflow errors.
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL,
                                  wintypes.LPCWSTR]
kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(wintypes.DWORD),
                                         wintypes.BOOL]
kernel32.CancelIo.argtypes = [wintypes.HANDLE]

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


def enumerate_paths(vid, pid):
    """Yield (path, caps) for every HID interface of a VID/PID."""
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    dev_info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if dev_info == INVALID_HANDLE:
        return

    try:
        index = 0
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(
                    dev_info, None, ctypes.byref(guid), index, ctypes.byref(iface)):
                return
            index += 1

            needed = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                dev_info, ctypes.byref(iface), None, 0, ctypes.byref(needed), None)
            buf = ctypes.create_string_buffer(needed.value)
            ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = (
                8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6)
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                    dev_info, ctypes.byref(iface), buf, needed, None, None):
                continue
            path = ctypes.wstring_at(
                ctypes.addressof(buf) + ctypes.sizeof(wintypes.DWORD))

            handle = kernel32.CreateFileW(
                path, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
            if handle == INVALID_HANDLE:
                continue
            try:
                attrs = HIDD_ATTRIBUTES()
                attrs.Size = ctypes.sizeof(attrs)
                if not hid.HidD_GetAttributes(handle, ctypes.byref(attrs)):
                    continue
                if attrs.VendorID != vid or attrs.ProductID != pid:
                    continue
                pp = ctypes.c_void_p()
                if not hid.HidD_GetPreparsedData(handle, ctypes.byref(pp)):
                    continue
                caps = HIDP_CAPS()
                hid.HidP_GetCaps(pp, ctypes.byref(caps))
                hid.HidD_FreePreparsedData(pp)
                yield path, caps
            finally:
                kernel32.CloseHandle(handle)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(dev_info)


class HidDevice:
    """One HID interface, opened for overlapped read and write."""

    def __init__(self, path, caps):
        self.path = path
        self.caps = caps
        self.handle = kernel32.CreateFileW(
            path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, None)
        if self.handle == INVALID_HANDLE:
            raise OSError("could not open %s" % path)
        self.event = kernel32.CreateEventW(None, True, False, None)

    def write(self, payload):
        """Send an output report. Returns True on success."""
        size = self.caps.OutputReportByteLength or (len(payload) + 1)
        buf = ctypes.create_string_buffer(size)
        buf[0] = 0  # report ID
        ctypes.memmove(ctypes.addressof(buf) + 1, bytes(payload),
                       min(len(payload), size - 1))
        return bool(hid.HidD_SetOutputReport(self.handle, buf, size))

    def read(self, timeout=1.0):
        """Read one input report, or None on timeout. Never blocks forever."""
        size = self.caps.InputReportByteLength
        if not size:
            return None
        buf = ctypes.create_string_buffer(size)
        overlapped = OVERLAPPED()
        overlapped.hEvent = self.event
        read = wintypes.DWORD(0)

        if kernel32.ReadFile(self.handle, buf, size, ctypes.byref(read),
                             ctypes.byref(overlapped)):
            return bytes(buf.raw[:read.value])

        if kernel32.WaitForSingleObject(
                self.event, int(timeout * 1000)) == WAIT_TIMEOUT:
            kernel32.CancelIo(self.handle)
            return None
        if not kernel32.GetOverlappedResult(
                self.handle, ctypes.byref(overlapped), ctypes.byref(read), False):
            return None
        return bytes(buf.raw[:read.value])

    def drain(self, seconds=1.5):
        """Collect every input report that arrives within a window."""
        reports = []
        end = time.time() + seconds
        while time.time() < end:
            report = self.read(timeout=max(0.05, end - time.time()))
            if report is None:
                break
            reports.append(report)
        return reports

    def close(self):
        if self.handle and self.handle != INVALID_HANDLE:
            kernel32.CancelIo(self.handle)
            kernel32.CloseHandle(self.handle)
            self.handle = None
        if self.event:
            kernel32.CloseHandle(self.event)
            self.event = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open_interface(vid, pid, mi=None, usage_page=None):
    """Open one interface of a device, selected by MI_xx and/or usage page."""
    for path, caps in enumerate_paths(vid, pid):
        lowered = path.lower()
        if mi is not None and ("mi_%02d" % mi) not in lowered:
            continue
        if usage_page is not None and caps.UsagePage != usage_page:
            continue
        return HidDevice(path, caps)
    return None
