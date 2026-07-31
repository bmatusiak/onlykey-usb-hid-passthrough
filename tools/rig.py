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

APP_IDS = "1D50:60FC"   # OnlyKey application firmware
BOOT_IDS = "16C0:0478"  # Teensy HalfKay bootloader

# Descriptors we expect to clone in application mode, in interface order. The
# order matters: host software locates raw HID by interface number, so rawhid
# must land at MI_02 exactly as it does on a real OnlyKey.
EXPECTED_INTERFACES = ["keyboard", "fido2", "rawhid", "seremu"]


def find_console(want=None):
    """Return the proxy's console port, or None.

    `want` restricts the search to a VID:PID, which is how a caller waits for a
    specific mode rather than just any console.
    """
    for port in list_ports.comports():
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
        if port not in [p.device for p in list_ports.comports()]:
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
    """Windows device-manager nodes for a VID:PID, as a list of DeviceIDs.

    Used to check what the PC actually bound, which is the only real proof the
    clone is correct -- our own counters cannot show that.
    """
    import subprocess
    vid, pid = ids.split(":")
    query = (
        "Get-CimInstance Win32_PnPEntity | "
        "Where-Object { $_.DeviceID -match 'VID_%s&PID_%s' } | "
        "Select-Object -ExpandProperty DeviceID" % (vid, pid))
    result = subprocess.run(["powershell", "-NoProfile", "-Command", query],
                            capture_output=True, text=True)
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]
