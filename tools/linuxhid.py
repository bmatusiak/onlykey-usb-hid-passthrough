"""Talk to one HID interface through Linux hidraw, with no dependencies.

The Linux replacement for the old winhid.py. Everything here is stdlib: hidraw
nodes are opened with os.open and polled with select, and the descriptor is read
from sysfs rather than through an ioctl, because sysfs also gives us the USB
interface number -- which is how an interface is selected on this rig.

    dev = open_interface(0x1D50, 0x60FC, mi=2)   # the OnlyKey's raw HID
    dev.write(payload)                            # one output report
    dev.drain(2.0)                                # every report within a window

Selecting by interface number
-----------------------------
The proxy clones the key's interface ORDER deliberately, so "raw HID is
interface 2" stays true through the passthrough and is the stable way to pick
one. /sys/class/hidraw/hidrawN/device/../bInterfaceNumber gives it directly.
Usage page works too and is checked the same way, from the report descriptor.

Report IDs
----------
None of the OnlyKey's four descriptors declare a report ID. That has two
consequences that bite if you assume Windows behaviour:

  * A hidraw WRITE still needs a leading report-ID byte, and it must be 0. So a
    64-byte report is a 65-byte write. write() adds that byte.
  * A hidraw READ does NOT carry one -- the 64 payload bytes arrive on their
    own. The Windows path prefixed reads with the report ID, so any code ported
    from it that skipped report[0] must stop skipping, or it eats a data byte.

Which wire path an output report takes
--------------------------------------
hidraw does not let you choose. The kernel sends an output report on the
interrupt OUT endpoint when the interface has one, and falls back to a
control-pipe SET_REPORT when it does not (usbhid returns -ENOSYS and hidraw
retries as a raw request). Both are handled by the proxy firmware -- see
set_report_thunk() in src/main.cpp -- but they are not the same path, so it is
worth knowing which one a given interface gets. describe() reports it.
"""
import os
import select
import time

SYSFS_HIDRAW = "/sys/class/hidraw"


# ------------------------------------------------------------ descriptor ----

class Caps:
    """The few descriptor facts callers actually use.

    Named to match the old Windows HIDP_CAPS fields so ported call sites read
    the same.
    """

    def __init__(self, usage_page=0, usage=0, in_len=0, out_len=0,
                 uses_report_ids=False):
        self.UsagePage = usage_page
        self.Usage = usage
        self.InputReportByteLength = in_len
        self.OutputReportByteLength = out_len
        self.uses_report_ids = uses_report_ids


def parse_report_descriptor(desc):
    """Return Caps for a HID report descriptor.

    A deliberately small parser: it tracks Report Size/Count across Input and
    Output main items and remembers the first top-level Usage Page/Usage. That
    covers every descriptor on this device. It does NOT handle per-report-ID
    lengths, so it asserts nothing about devices that use report IDs beyond
    setting the flag -- callers on this rig do not have any.
    """
    usage_page = usage = 0
    # Latched at the first Collection. Usage Page is a GLOBAL item, so it keeps
    # being reassigned as the descriptor walks through other pages -- the
    # keyboard's top level is Generic Desktop (0x01) but its last Usage Page is
    # Keyboard/Keypad (0x07). Reading the running value at the end reports the
    # wrong page for exactly the interfaces that have more than one.
    top_page = top_usage = 0
    seen_top = False
    size = count = 0
    in_bits = out_bits = 0
    uses_report_ids = False

    i = 0
    while i < len(desc):
        prefix = desc[i]
        i += 1
        length = prefix & 0x03
        if length == 3:
            length = 4
        tag = (prefix >> 4) & 0x0F
        typ = (prefix >> 2) & 0x03
        data = desc[i:i + length]
        i += length
        value = 0
        for shift, byte in enumerate(data):
            value |= byte << (8 * shift)

        if typ == 1:            # Global
            if tag == 0x0:      # Usage Page
                usage_page = value
            elif tag == 0x7:    # Report Size
                size = value
            elif tag == 0x8:    # Report ID
                uses_report_ids = True
            elif tag == 0x9:    # Report Count
                count = value
        elif typ == 2:          # Local
            if tag == 0x0 and not seen_top:   # Usage
                usage = value
        elif typ == 0:          # Main
            if tag == 0x8:      # Input
                in_bits += size * count
            elif tag == 0x9:    # Output
                out_bits += size * count
            if tag == 0xA and not seen_top:   # Collection
                top_page, top_usage = usage_page, usage
                seen_top = True

    def to_bytes(bits):
        if not bits:
            return 0
        # +1 for the report-ID byte the OS prepends when the device uses them.
        return (bits + 7) // 8 + (1 if uses_report_ids else 0)

    return Caps(top_page, top_usage, to_bytes(in_bits), to_bytes(out_bits),
                uses_report_ids)


# ------------------------------------------------------------- discovery ----

def _read(path, binary=False):
    try:
        with open(path, "rb" if binary else "r") as fh:
            return fh.read()
    except OSError:
        return None


def enumerate_interfaces(vid, pid):
    """Every hidraw node belonging to a VID/PID, as a list of dicts.

    Keys: path, mi (USB interface number, or None), caps, has_out_ep.
    Sorted by interface number so the order is stable across boots -- hidrawN
    numbering is not.
    """
    found = []
    try:
        nodes = sorted(os.listdir(SYSFS_HIDRAW))
    except OSError:
        return found

    for node in nodes:
        base = os.path.join(SYSFS_HIDRAW, node, "device")
        uevent = _read(os.path.join(base, "uevent")) or ""
        # HID_ID=0003:00001D50:000060FC -- bus:vendor:product, all hex.
        ids = None
        for line in uevent.splitlines():
            if line.startswith("HID_ID="):
                parts = line.split("=", 1)[1].split(":")
                if len(parts) == 3:
                    ids = (int(parts[1], 16), int(parts[2], 16))
        if ids != (vid, pid):
            continue

        desc = _read(os.path.join(base, "report_descriptor"), binary=True) or b""
        itf = _read(os.path.join(base, "..", "bInterfaceNumber"))
        found.append({
            "path": "/dev/" + node,
            "mi": int(itf.strip(), 16) if itf else None,
            "caps": parse_report_descriptor(desc),
            "has_out_ep": _has_out_endpoint(os.path.join(base, "..")),
        })

    found.sort(key=lambda d: (d["mi"] is None, d["mi"]))
    return found


def _has_out_endpoint(iface_dir):
    """True if this USB interface has an interrupt OUT endpoint.

    Decides whether an output report goes out on the interrupt endpoint or
    falls back to a control-pipe SET_REPORT. HalfKay having no OUT endpoint is
    what makes a 1088-byte firmware block a control transfer.
    """
    try:
        for entry in os.listdir(iface_dir):
            # Endpoint dirs are named ep_81, ep_02, ... -- bit 7 clear is OUT.
            if not entry.startswith("ep_"):
                continue
            addr = _read(os.path.join(iface_dir, entry, "bEndpointAddress"))
            if addr is None:
                continue
            if not int(addr.strip(), 16) & 0x80:
                return True
    except OSError:
        pass
    return False


def describe(vid, pid):
    """Human-readable summary of every interface. Used by tools to print state."""
    lines = []
    for itf in enumerate_interfaces(vid, pid):
        caps = itf["caps"]
        lines.append(
            "%s  mi=%s usage_page=0x%04X usage=0x%04X in=%d out=%d  "
            "output goes via %s"
            % (itf["path"], itf["mi"], caps.UsagePage, caps.Usage,
               caps.InputReportByteLength, caps.OutputReportByteLength,
               "interrupt OUT" if itf["has_out_ep"] else "control SET_REPORT"))
    return lines


# ---------------------------------------------------------------- device ----

class HidDevice:
    """One HID interface, opened non-blocking for read and write."""

    def __init__(self, path, caps, has_out_ep=False, mi=None):
        self.path = path
        self.caps = caps
        self.has_out_ep = has_out_ep
        self.mi = mi
        # Why a write failed matters more than that it did: EIO from a wedged
        # proxy and EINVAL from a wrong-sized report need different responses.
        self.last_error = None
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

    def write(self, payload, report_id=0):
        """Send one output report. Returns True on success.

        The report-ID byte is prepended here; pass the bare payload. The report
        is zero-padded to the descriptor's output length when there is one, so
        the device always sees a full-size report.
        """
        payload = bytes(payload)
        size = self.caps.OutputReportByteLength
        if size:
            # OutputReportByteLength counts the ID byte only when the device
            # uses report IDs; normalise to a payload length either way.
            body = size - 1 if self.caps.uses_report_ids else size
            payload = payload[:body] + b"\x00" * max(0, body - len(payload))
        try:
            os.write(self.fd, bytes([report_id]) + payload)
            self.last_error = None
            return True
        except OSError as exc:
            self.last_error = exc
            return False

    def read(self, timeout=1.0):
        """Read one input report, or None on timeout. Never blocks forever."""
        ready, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not ready:
            return None
        try:
            # Read generously: the kernel returns one whole report per read.
            return os.read(self.fd, 4096)
        except OSError:
            return None

    def drain(self, seconds=1.5):
        """Collect every input report that arrives within a window."""
        reports = []
        end = time.time() + seconds
        while True:
            remaining = end - time.time()
            if remaining <= 0:
                break
            report = self.read(timeout=max(0.05, remaining))
            if report is None:
                break
            reports.append(report)
        return reports

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open_interface(vid, pid, mi=None, usage_page=None):
    """Open one interface of a device, selected by interface number and/or usage.

    Returns None if nothing matches, so callers can report the failure with
    their own context rather than catching.
    """
    for itf in enumerate_interfaces(vid, pid):
        if mi is not None and itf["mi"] != mi:
            continue
        if usage_page is not None and itf["caps"].UsagePage != usage_page:
            continue
        return HidDevice(itf["path"], itf["caps"], itf["has_out_ep"], itf["mi"])
    return None


if __name__ == "__main__":
    import sys
    vid = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1D50
    pid = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x60FC
    lines = describe(vid, pid)
    print("%04X:%04X -- %d interface(s)" % (vid, pid, len(lines)))
    for line in lines:
        print("  " + line)
