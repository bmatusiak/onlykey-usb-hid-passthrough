"""Truly power the attached key down, from the host, with nothing to press.

This is the only recovery that is *provably* a cold power cycle, and it works by
routing through the RP2040's own ROM bootloader:

  1. 1200-baud touch  -> the Feather enters BOOTSEL
  2. in BOOTSEL every GPIO is at its reset state, so 5V_EN is undriven AND the
     PIO has stopped driving D+/D-. The key loses power completely -- measured,
     it disappears from the bus entirely, 0 devices enumerated
  3. wait as long as you like; unlike a watchdog reset this is not a few
     milliseconds of undriven pads
  4. picotool loads the already-built UF2 -> the RP2040 leaves BOOTSEL and runs
     the proxy again, VBUS returns, and the key boots from genuinely dead

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

BOOTSEL is detected by the RP2040's ROM bootloader appearing on the USB bus as
2E8A:0003, not by waiting for the RPI-RP2 drive to mount. Nothing here needs the
drive mounted, which matters because this rig does not automount it -- and the
device being on the bus is the more direct evidence anyway.

Leaving BOOTSEL uses picotool, which on Linux talks to the ROM bootloader
through libusb and needs only a udev rule for 2E8A -- no driver install and no
mount. It is the same operation `pio run -t upload` performs.

Exit 0 if the key came back, 1 otherwise.
"""
import argparse
import os
import subprocess
import sys
import time

import rig

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UF2 = os.path.join(ROOT, ".pio", "build",
                   "adafruit_feather_rp2040_usb_host", "firmware.uf2")


def leave_bootsel(uf2):
    """Load the UF2 and run it. Returns None on success, else a message."""
    tool = rig.picotool()
    if not tool:
        return ("no picotool found -- install it, or build once with PlatformIO "
                "so the core's copy is unpacked")
    result = subprocess.run([tool, "load", "-x", uf2],
                            capture_output=True, text=True)
    if result.returncode != 0:
        # Almost always the 2E8A udev rule: without it the ROM bootloader is
        # readable but not claimable, and picotool fails on open, not on find.
        return ("picotool failed (%d): %s"
                % (result.returncode,
                   (result.stderr or result.stdout).strip()))
    return None


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

    if not rig.wait_for_bootsel(timeout=15):
        print("the Feather never reached BOOTSEL")
        return 1
    print("BOOTSEL (%s) -- the key is now unpowered" % rig.BOOTSEL_IDS)

    # Proof, not assumption: nothing of the key should be on the bus at all.
    remaining = len(rig.usb_nodes(rig.APP_IDS)) + len(rig.usb_nodes(rig.BOOT_IDS))
    print("key nodes on the bus while unpowered: %d%s"
          % (remaining, "" if remaining == 0 else "  <-- expected 0"))

    print("holding %.1f s" % args.hold)
    time.sleep(args.hold)

    problem = leave_bootsel(args.uf2)
    if problem:
        print(problem)
        return 1
    print("UF2 loaded; the proxy is restarting")

    # Where it lands is decided by the key's firmware, not by this tool: valid
    # firmware cold-boots into the application, an invalidated image has nowhere
    # to go but the bootloader. GPIO 6's reset pull-down does NOT force the
    # bootloader -- in BOOTSEL every pad sits at reset state through the key's
    # whole power-up and it still comes back in application mode, measured here
    # and again on Linux.
    #
    # So wait for EITHER rather than guessing an order. Waiting on the
    # bootloader first cost a 45 s timeout on the ordinary path, every time.
    port = rig.wait_for_mode(None, timeout=60)
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
