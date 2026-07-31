"""Soak test: cycle the key between application and bootloader mode repeatedly.

This exercises the exact window the host stack used to wedge in -- device detach
and re-enumeration -- and checks after every cycle that core 1 is still running,
that nothing was dropped, and whether the PIO bus desynced. An unattended rig is
only worth having if it survives being driven hard, and "it worked once" is not
evidence of that.

    python soak.py         10 cycles
    python soak.py 50      50 cycles

EVERY CYCLE FLASHES, and that is not a slow-but-thorough choice -- it is the
only thing that works. Entering the OnlyKey's bootloader invalidates its
application firmware, so HalfKay has nothing to jump back to and a bare
`--boot-only` reboot returns the key to the bootloader, not to the application.
An earlier version of this script used reboot-only and failed on cycle 1 every
time, which looked convincingly like a passthrough bug and was not one.

Each cycle costs a full 210-block flash, ~20 s.

Exit 0 if every cycle passed, 1 on the first failure -- which is the point: stop
at the failure, with the cycle number and the phase core 1 died in, rather than
producing a long log nobody reads.
"""
import argparse
import os
import sys
import time

import rig

HEX_DEFAULT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "firmware.hex")


def fail(cycle, message, port=None):
    print("\nFAILED on cycle %d: %s" % (cycle, message))
    if port:
        # The human-readable block names the phase core 1 died in, which is the
        # single most useful thing to know and is gone once anything else
        # touches the device.
        print(rig.status(port))
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cycles", nargs="?", type=int, default=10)
    ap.add_argument("--hex", default=HEX_DEFAULT,
                    help="image to flash each cycle (flashing is mandatory)")
    args = ap.parse_args()

    port = rig.find_console()
    if not port:
        print("no proxy console found")
        return 1

    print("soak: %d cycles on %s, flashing %s each time"
          % (args.cycles, port, os.path.basename(args.hex)))
    started = time.time()
    desyncs = 0

    for cycle in range(1, args.cycles + 1):
        # --- into the bootloader -------------------------------------------
        problem = rig.ensure_application(args.hex)
        if problem:
            return fail(cycle, "could not start from a working key: %s" % problem,
                        rig.find_console())
        port = rig.find_console(rig.APP_IDS)
        before = (rig.health(port) or {}).get("piotimeouts", "0")

        port = rig.enter_bootloader()
        if not port:
            return fail(cycle, "key never appeared in bootloader mode",
                        rig.find_console())

        state = rig.health(port)
        if not state:
            return fail(cycle, "no health line in bootloader mode", port)
        if state.get("core1") != "ok":
            return fail(cycle, "core1=%s in bootloader mode" % state.get("core1"),
                        port)

        # --- back to the application ---------------------------------------
        # Handles all three states, including a descriptor-less key, and
        # re-flashes on a proxy fault (exit 2: a PIO desync abandoned a
        # transaction, so the image cannot be trusted).
        problem = rig.ensure_application(args.hex)
        if problem:
            return fail(cycle, problem, rig.find_console())

        port = rig.find_console(rig.APP_IDS)
        state = rig.health(port)
        if not state:
            return fail(cycle, "no health line in application mode", port)
        if state.get("core1") != "ok":
            return fail(cycle, "core1=%s after reboot" % state.get("core1"), port)
        if state.get("dropflag") != "0":
            return fail(cycle, "reports were dropped after being ACKed", port)
        if state.get("cloneerr", "0") != "0":
            return fail(cycle, "an interface failed to clone (cloneerr=%s)"
                        % state.get("cloneerr"), port)
        if state.get("piotimeouts") != before:
            desyncs += 1

        print("  cycle %2d/%d ok   piotimeouts=%s itf=%s"
              % (cycle, args.cycles, state.get("piotimeouts", "?"),
                 state.get("itf", "?")))
        sys.stdout.flush()

    print("\nall %d cycles passed in %.0f s (%d cycle(s) saw a PIO desync)"
          % (args.cycles, time.time() - started, desyncs))
    final = rig.health(rig.find_console())
    if final:
        print("final: %s" % final)
    return 0


if __name__ == "__main__":
    sys.exit(main())
