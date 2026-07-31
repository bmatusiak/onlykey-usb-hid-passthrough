"""Truly power the attached key down, from the host, with nothing to press.

This is the only recovery that is *provably* a cold power cycle, and it works by
routing through the RP2040's own ROM bootloader:

  1. 1200-baud touch  -> the Feather enters BOOTSEL
  2. in BOOTSEL every GPIO is at its reset state, so 5V_EN is undriven AND the
     PIO has stopped driving D+/D-. The key loses power completely -- measured,
     it disappears from the bus entirely, 0 devices enumerated
  3. wait as long as you like; unlike a watchdog reset this is not a few
     milliseconds of undriven pads
  4. copy the already-built UF2 -> the RP2040 leaves BOOTSEL and runs the proxy
     again, VBUS returns, and the key boots from genuinely dead

Why this matters: cutting VBUS from firmware is NOT a power cycle. The PIO keeps
driving D+/D- while the rail is down and back-feeds the key through its
protection diodes, so it never re-runs its startup. That single fact explains
why every VBUS-based recovery failed while an RP2040 reset always worked.

Where the key LANDS afterwards depends on its firmware, not on this tool:

  valid firmware  -> cold-boots into application mode
  invalidated     -> bootloader, because there is nowhere else to go

Both observed minutes apart. If you want the bootloader from a working key, use
`b` afterwards; this tool only guarantees the power cycle.

    python coldcycle.py              cycle, then report where the key landed
    python coldcycle.py --hold 8     hold the key unpowered for 8 s

`picotool reboot -a` would be the tidier way out of BOOTSEL, but on Windows it
needs a WinUSB driver installed via Zadig, which displaces the RPI-RP2 drive the
flashing path depends on. Copying the UF2 needs no setup and is the same
operation `pio run -t upload` already performs.

Exit 0 if the key came back, 1 otherwise.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

import rig

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UF2 = os.path.join(ROOT, ".pio", "build",
                   "adafruit_feather_rp2040_usb_host", "firmware.uf2")


def bootsel_drive():
    """Return the RPI-RP2 drive letter, or None."""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_LogicalDisk | "
         "Where-Object { $_.VolumeName -eq 'RPI-RP2' } | "
         "Select-Object -ExpandProperty DeviceID"],
        capture_output=True, text=True)
    drive = result.stdout.strip()
    return drive or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hold", type=float, default=5.0,
                    help="seconds to leave the key unpowered")
    ap.add_argument("--uf2", default=UF2, help="image used to exit BOOTSEL")
    args = ap.parse_args()

    if not os.path.isfile(args.uf2):
        print("no UF2 at %s -- build first (pio run)" % args.uf2)
        return 1

    port = rig.find_console()
    if not port:
        print("no proxy console found")
        return 1
    print("console: %s" % port)

    subprocess.run([sys.executable, os.path.join(HERE, "touch1200.py"), port],
                   capture_output=True, text=True)

    drive = None
    deadline = time.time() + 15
    while time.time() < deadline and not drive:
        time.sleep(0.5)
        drive = bootsel_drive()
    if not drive:
        print("the Feather never reached BOOTSEL")
        return 1
    print("BOOTSEL on %s -- the key is now unpowered" % drive)

    # Proof, not assumption: nothing of the key should be on the bus at all.
    remaining = len(rig.usb_nodes(rig.APP_IDS)) + len(rig.usb_nodes(rig.BOOT_IDS))
    print("key nodes on the bus while unpowered: %d%s"
          % (remaining, "" if remaining == 0 else "  <-- expected 0"))

    print("holding %.1f s" % args.hold)
    time.sleep(args.hold)

    shutil.copyfile(args.uf2, os.path.join(drive + "\\", "firmware.uf2"))
    print("UF2 copied; the proxy is restarting")

    # The key powers up with the contact pressed (GPIO 6 reset pull-down) and
    # released as the proxy boots, so it should land in its bootloader.
    port = rig.wait_for_mode(rig.BOOT_IDS, timeout=45) or \
        rig.wait_for_mode(rig.APP_IDS, timeout=15)
    if not port:
        print("the key did not come back")
        return 1

    state = rig.health(port) or {}
    print("key is back: %s:%s, itf=%s, core1=%s"
          % (state.get("vid"), state.get("pid"), state.get("itf"),
             state.get("core1")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
