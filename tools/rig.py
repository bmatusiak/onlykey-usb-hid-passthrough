"""Shared helpers for driving the Feather proxy from a script.

Factored out because selftest.py, soak.py and anything else that automates the
rig all need the same three things: find the console (whose COM number moves
when the key changes mode), send it a command, and read its health line. Three
copies of that logic would drift, and the port-handling rules below are exactly
the sort of thing that gets subtly wrong when duplicated.

RULES, learned the hard way:
  - Only ONE process may hold the console. A second opener gets
    PermissionError(13) and whatever was being captured is lost.
  - The console's COM number CHANGES with the key's mode, because the proxy
    adopts the key's VID/PID and re-enumerates. Never cache it across a mode
    change; call find_console() again.
  - The port vanishing during a mode change is normal, not an error.
"""
import time

import serial
from serial.tools import list_ports

APP_IDS = "1D50:60FC"     # OnlyKey application firmware
BOOT_IDS = "16C0:0478"    # Teensy HalfKay bootloader
BOOTSEL_IDS = "2E8A:0003"  # the RP2040's own ROM bootloader (the Feather, not
                           # the key) -- a third identity the rig passes
                           # through, and the one a USB filter is most likely
                           # to be missing.

# Descriptors we expect to clone in application mode, in interface order. The
# order matters: host software locates raw HID by interface number, so rawhid
# must land at MI_02 exactly as it does on a real OnlyKey.
EXPECTED_INTERFACES = ["keyboard", "fido2", "rawhid", "seremu"]


def comports():
    """list_ports.comports(), retried through a device disappearing mid-scan.

    pyserial's Linux backend reads idVendor/idProduct out of sysfs and calls
    int(read_line(...), 16) without checking the None that read_line returns
    when the file has already gone. So a device unplugging DURING a scan raises
    TypeError from inside comports() instead of simply not being listed:

        TypeError: int() can't convert non-string with explicit base

    The calls most likely to hit it are precisely the ones that exist to watch
    for a device disappearing -- wait_for_gone() polls this every 0.2 s while a
    watchdog reset takes the board away, and hit it reliably. Windows never saw
    this because that backend uses SetupAPI, not sysfs.

    Retrying is enough: the node is gone by the next pass.
    """
    for _ in range(5):
        try:
            return list(list_ports.comports())
        except (TypeError, OSError):
            time.sleep(0.1)
    return []


def find_console(want=None):
    """Return the proxy's console port, or None.

    `want` restricts the search to a VID:PID, which is how a caller waits for a
    specific mode rather than just any console.
    """
    for port in comports():
        hwid = (port.hwid or "").upper()
        if want:
            if ("VID:PID=" + want) in hwid:
                return port.device
        elif any(("VID:PID=" + k) in hwid for k in (APP_IDS, BOOT_IDS)):
            return port.device
    return None


def wait_for_mode(want, timeout=30):
    """Wait for the proxy to re-enumerate in the given mode. Returns the port."""
    end = time.time() + timeout
    while time.time() < end:
        port = find_console(want)
        if port:
            # Windows exposes the port slightly before it is openable.
            time.sleep(1.0)
            return port
        time.sleep(0.3)
    return None


def wait_for_gone(port, timeout=15):
    """Wait for a port to disappear, e.g. after triggering a mode change."""
    end = time.time() + timeout
    while time.time() < end:
        if port not in [p.device for p in comports()]:
            return True
        time.sleep(0.2)
    return False


def send(port, keys, wait=1.2):
    """Send console keys and return everything the board said.

    Never raises: the port disappearing mid-command is the expected outcome of
    any command that changes the key's mode.
    """
    try:
        ser = serial.Serial(port, 115200, timeout=0.05)
    except Exception as exc:
        return "OPEN FAILED: %s" % exc
    out = b""
    try:
        for key in keys:
            ser.write(key.encode())
            ser.flush()
            time.sleep(0.25)
        end = time.time() + wait
        while time.time() < end:
            out += ser.read(4096)
    except Exception:
        pass
    try:
        ser.close()
    except Exception:
        pass
    return out.decode("utf-8", "replace")


def health(port, retries=3):
    """Parse the `s` health line into a dict, or None."""
    for _ in range(retries):
        for line in send(port, "s").splitlines():
            if "[health]" in line:
                fields = {}
                for token in line.split("[health]", 1)[1].split():
                    if "=" in token:
                        key, value = token.split("=", 1)
                        fields[key] = value
                if fields:
                    return fields
    return None


def status(port):
    """The human-readable `i` block. Use for diagnosis, never for parsing."""
    return send(port, "i", wait=2.0)


def enter_bootloader(timeout=90):
    """Get the key into HalfKay with its HID interface bound. Returns the port.

    Not just "send b and wait". Two things make this slower and less certain
    than it looks:

      - The key often lands DESCRIPTOR-LESS first: enumerated as 16C0:0478 with
        the console present, but `itf=0` and no HID interface to flash. It needs
        another contact pulse to present its descriptors.
      - The proxy's own auto-pulse deliberately holds off
        PROXY_BOOTSEL_REBOOT_GRACE_MS (30 s) once a device has been seen, so
        that a key rebooting into freshly flashed firmware is not knocked
        straight back into the bootloader. Correct, but far too slow to wait on
        when the bootloader is what we actually want.

    So pulse again ourselves rather than waiting for the proxy to give up.
    Returns None if the key never presents a bootloader interface.
    """
    deadline = time.time() + timeout
    pulses = 0

    while time.time() < deadline:
        port = find_console()
        if not port:
            time.sleep(0.5)
            continue

        state = health(port)
        if not state:
            time.sleep(0.5)
            continue

        # There, with a HID interface bound: ready to flash.
        if state.get("pid") == "0478" and state.get("itf", "0") != "0":
            return port

        # Either still in the application, or bootloader-but-descriptor-less.
        # These need DIFFERENT things, and using the wrong one silently does
        # nothing:
        #
        #   running firmware  -> `b`, a tap the application notices
        #   descriptor-less   -> `B`, a cold entry with the contact held across
        #                        a power cycle, because the contact is sampled
        #                        while booting and there is no firmware left to
        #                        notice a tap
        if pulses >= 5:
            return None
        pulses += 1
        send(port, "B" if state.get("itf", "0") == "0" else "b")
        time.sleep(9)  # cold entry includes a full power cycle

    return None


def flash(hexfile, attempts=3, extra=None):
    """Flash the key, retrying a proxy fault. Returns (returncode, output).

    Exit 2 means the proxy lost data or the PIO bus desynced mid-flash, so the
    image cannot be trusted -- but that is a bus event, not a defect in the
    image, and the correct response is to flash again. Retrying here is what
    makes the rig unattended. Exit 1 is a real flash error and is never retried.
    """
    import os
    import subprocess
    import sys

    here = os.path.dirname(os.path.abspath(__file__))
    args = [sys.executable, os.path.join(here, "halfkay_flash.py")]
    args += extra if extra else [hexfile]

    output = ""
    for attempt in range(1, attempts + 1):
        result = subprocess.run(args, capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 2:
            return result.returncode, output
        if attempt < attempts:
            time.sleep(2)
    return 2, output


def ensure_application(hexfile, timeout=120):
    """Get the key back to application mode from whatever state it is in.

    Three states, not two:

      - application already: done.
      - bootloader with its HID interface bound: flash back.
      - enumerated but DESCRIPTOR-LESS (`itf=0`): the key came up without
        presenting any HID interface, so there is nothing to flash. A contact
        pulse wakes it. This happens after a bare reboot out of HalfKay and is
        easy to mistake for a dead key -- the console is still there and the
        VID/PID is still right; only `itf=0` gives it away.

    This is the rig's recovery routine. If it cannot reach a working key without
    hands, the rig is not unattended. Returns None on success, else a reason.
    """
    deadline = time.time() + timeout
    pulses = 0

    while time.time() < deadline:
        port = find_console(APP_IDS)
        if port:
            state = health(port)
            if state and state.get("itf") == "4":
                return None

        port = find_console()
        if not port:
            time.sleep(1)
            continue
        state = health(port)
        if not state:
            time.sleep(1)
            continue

        if state.get("itf") == "0":
            # Descriptor-less: the contact is sampled while booting, so a tap
            # does nothing here -- it needs a cold entry with the contact held
            # across a power cycle. Done here rather than waiting for the
            # proxy's own recovery, which holds off 30 s once a device has been
            # seen.
            if pulses >= 4:
                return ("key stays descriptor-less after %d cold entries"
                        % pulses)
            pulses += 1
            send(port, "B")
            time.sleep(10)
            continue

        if state.get("pid") == "0478":
            code, out = flash(hexfile)
            if code != 0:
                return "could not flash back to application mode: %s" % out
            if not wait_for_mode(APP_IDS, timeout=40):
                return "key did not return to application mode after flashing"
            continue

        time.sleep(1)

    return "could not reach application mode within %ds" % timeout


def usb_nodes(ids):
    """Kernel-bound USB interfaces for a VID:PID, one string per interface.

    Used to check what the PC actually bound, which is the only real proof the
    clone is correct -- our own counters cannot show that. Reading sysfs rather
    than lsusb keeps this dependency-free and, more usefully, reports the DRIVER
    that claimed each interface: an interface the kernel enumerated but bound
    nothing to is a different failure from one that never appeared.

    Each entry looks like "1-1:1.2 MI_02 driver=usbhid hidraw2".
    """
    import os

    vid, pid = (int(part, 16) for part in ids.split(":"))
    root = "/sys/bus/usb/devices"
    nodes = []

    def read(*parts):
        try:
            with open(os.path.join(*parts)) as fh:
                return fh.read().strip()
        except OSError:
            return None

    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return nodes

    for entry in entries:
        # Interfaces are named "1-1:1.0" and appear under their parent device;
        # skip them here so each is reported exactly once.
        if ":" in entry:
            continue
        path = os.path.join(root, entry)
        have_vid = read(path, "idVendor")
        have_pid = read(path, "idProduct")
        if not have_vid or not have_pid:
            continue
        if int(have_vid, 16) != vid or int(have_pid, 16) != pid:
            continue

        for sub in sorted(os.listdir(path)):
            if not sub.startswith(entry + ":"):
                continue
            ipath = os.path.join(path, sub)
            number = read(ipath, "bInterfaceNumber")
            try:
                driver = os.path.basename(os.readlink(os.path.join(ipath, "driver")))
            except OSError:
                driver = "none"
            # The hidraw node hangs below the interface via the hid device, so
            # its depth varies; walk rather than guess the path.
            hidraws = []
            for parent, _, _ in os.walk(ipath):
                if os.path.basename(parent) == "hidraw":
                    hidraws.extend(sorted(os.listdir(parent)))
            nodes.append("%s MI_%02d driver=%s%s"
                         % (sub, int(number, 16) if number else -1, driver,
                            (" " + " ".join(hidraws)) if hidraws else ""))
    return nodes


def in_bootsel():
    """True if the Feather is sitting in the RP2040 ROM bootloader.

    Presence on the USB bus, not a mounted RPI-RP2 drive: this rig does not
    automount, and picotool does not need the drive anyway. It is also the more
    direct evidence -- the drive appearing is a consequence of the device being
    there, and it can lag by seconds.
    """
    return bool(usb_nodes(BOOTSEL_IDS))


def wait_for_bootsel(timeout=15):
    """Wait for BOOTSEL. Returns True if it arrived."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if in_bootsel():
            return True
        time.sleep(0.5)
    return False


def picotool():
    """Path to picotool, or None.

    The earlephilhower core ships one, which is preferred over anything on PATH
    so the version always matches the toolchain that produced the UF2.
    """
    import os

    bundled = os.path.expanduser(
        "~/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool")
    if os.path.isfile(bundled) and os.access(bundled, os.X_OK):
        return bundled
    from shutil import which
    return which("picotool")
