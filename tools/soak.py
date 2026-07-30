"""Soak test: cycle the key between application and bootloader mode repeatedly.

This exercises the exact window the host stack used to wedge in -- device detach
and re-enumeration -- and checks after every cycle that core 1 is still running.
An unattended rig is only worth having if it survives being driven hard, and
"it worked once" is not evidence of that.

    python soak.py         10 cycles
    python soak.py 50      50 cycles

EVERY CYCLE FLASHES, and that is not a slow-but-thorough choice -- it is the
only thing that works. Entering the OnlyKey's bootloader invalidates its
application firmware, so HalfKay has nothing to jump back to and a bare
`--boot-only` reboot returns the key to the bootloader, not to the application.
An earlier version of this script used reboot-only and failed on cycle 1 every
time, which looked convincingly like a passthrough bug and was not one.

Each cycle therefore costs a full 210-block flash, ~20 s.

Exit 0 if every cycle passed, 1 on the first failure -- which is the point: stop
at the failure, with the cycle number and the phase core 1 died in, rather than
producing a long log nobody reads.
"""
import argparse
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

APP_IDS = "1D50:60FC"      # OnlyKey application firmware
BOOT_IDS = "16C0:0478"     # HalfKay bootloader
HERE = __file__.rsplit("\\", 1)[0] if "\\" in __file__ else "."


def find_console(want=None):
    """Return the proxy console, optionally only when it shows given IDs."""
    for port in list_ports.comports():
        hwid = (port.hwid or "").upper()
        if want:
            if ("VID:PID=" + want) in hwid:
                return port.device
        elif any(("VID:PID=" + k) in hwid for k in (APP_IDS, BOOT_IDS)):
            return port.device
    return None


def wait_for(want, timeout=25):
    """Wait for the proxy to re-enumerate with the given IDs."""
    end = time.time() + timeout
    while time.time() < end:
        port = find_console(want)
        if port:
            time.sleep(1.0)  # let the console settle before opening it
            return port
        time.sleep(0.4)
    return None


def console(port, keys, wait=1.2):
    """Send keys, return everything the board said."""
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
        pass  # the port vanishing mid-command is expected on a mode change
    try:
        ser.close()
    except Exception:
        pass
    return out.decode("utf-8", "replace")


def health(port):
    for line in console(port, "s").splitlines():
        if "[health]" in line:
            fields = {}
            for token in line.split("[health]", 1)[1].split():
                if "=" in token:
                    k, v = token.split("=", 1)
                    fields[k] = v
            return fields
    return None


def fail(cycle, message, port=None):
    print("\nFAILED on cycle %d: %s" % (cycle, message))
    if port:
        # The human-readable block names the phase core 1 died in, which is the
        # single most useful thing to know here.
        print(console(port, "i", wait=2.0))
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cycles", nargs="?", type=int, default=10)
    ap.add_argument("--hex", default="firmware.hex",
                    help="image to flash each cycle (flashing is mandatory)")
    args = ap.parse_args()

    port = find_console()
    if not port:
        print("no proxy console found")
        return 1

    print("soak: %d cycles on %s, flashing %s each time"
          % (args.cycles, port, args.hex))
    started = time.time()

    for cycle in range(1, args.cycles + 1):
        # --- into the bootloader -------------------------------------------
        console(port, "b")
        port = wait_for(BOOT_IDS)
        if not port:
            return fail(cycle, "key never appeared in bootloader mode")

        state = health(port)
        if not state:
            return fail(cycle, "no health line in bootloader mode", port)
        if state.get("core1") != "ok":
            return fail(cycle, "core1=%s in bootloader mode"
                        % state.get("core1"), port)

        # --- back to the application ---------------------------------------
        # Must be a real flash: entering the bootloader invalidated the app, so
        # there is nothing for a bare reboot to return to.
        result = subprocess.run(
            [sys.executable, HERE + "\\halfkay_flash.py", args.hex],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, result.stderr)
            return fail(cycle, "flasher exited %d" % result.returncode)

        port = wait_for(APP_IDS)
        if not port:
            return fail(cycle, "key never returned to application mode")

        state = health(port)
        if not state:
            return fail(cycle, "no health line in application mode", port)
        if state.get("core1") != "ok":
            return fail(cycle, "core1=%s after reboot" % state.get("core1"), port)
        if state.get("dropflag") != "0":
            return fail(cycle, "reports were dropped", port)

        print("  cycle %2d/%d ok   piotimeouts=%s itf=%s"
              % (cycle, args.cycles, state.get("piotimeouts", "?"),
                 state.get("itf", "?")))
        sys.stdout.flush()

    print("\nall %d cycles passed in %.0f s" % (args.cycles, time.time() - started))
    final = health(port)
    if final:
        print("final: %s" % final)
    return 0


if __name__ == "__main__":
    sys.exit(main())
