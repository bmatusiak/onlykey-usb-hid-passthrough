"""Scenario tests for the Feather proxy.

The rig is meant to run unattended, so "it worked when I tried it" is not good
enough -- every path an LLM might drive has to be exercised deliberately, and
each one has to fail loudly rather than degrade quietly.

    python selftest.py              run everything
    python selftest.py --list       show the scenarios
    python selftest.py -k power     run only matching scenarios

Each scenario returns None on success or a string describing the failure. The
run stops at the first failure and prints the `i` block, because the phase core
1 died in is the single most useful thing to know and it is gone once anything
else touches the device.

Scenarios that end with the key in the bootloader always flash it back:
entering the bootloader invalidates the application firmware, so leaving the
rig there would strand it for the next caller.

Exit 0 if everything passed, 1 otherwise.
"""
import argparse
import os
import subprocess
import sys
import time

import rig

HERE = os.path.dirname(os.path.abspath(__file__))
HEX = os.path.join(os.path.dirname(HERE), "firmware.hex")


def flash(extra=None, attempts=3):
    """Flash the key, retrying a proxy fault. See rig.flash().

    Fault-injection scenarios pass attempts=1: they are asserting that the
    flasher REFUSES, so retrying would paper over exactly what is under test.
    """
    return rig.flash(HEX, extra=extra, attempts=attempts)


def ensure_application():
    """Leave the key in application mode. See rig.ensure_application()."""
    return rig.ensure_application(HEX)


# --------------------------------------------------------------- scenarios --

def test_console_and_health():
    """The console answers and the health line has every field a script needs."""
    port = rig.find_console()
    if not port:
        return "no proxy console found"
    state = rig.health(port)
    if not state:
        return "no [health] line -- scripts have nothing to gate on"
    required = ["core1", "drops", "dropflag", "sent", "in", "indrops",
                "piotimeouts", "itf", "mounted", "vid", "pid"]
    missing = [f for f in required if f not in state]
    if missing:
        return "health line missing fields: %s" % ", ".join(missing)
    if state["core1"] != "ok":
        return "core1=%s" % state["core1"]
    return None


def test_application_clone():
    """All four OnlyKey interfaces are cloned, in the right order."""
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)
    block = rig.status(port)

    for index, name in enumerate(rig.EXPECTED_INTERFACES):
        marker = "slot %d" % index
        line = next((l for l in block.splitlines() if marker in l), None)
        if not line:
            return "no %s in status" % marker
        if name not in line:
            return "slot %d should be %s, got: %s" % (index, name, line.strip())

    state = rig.health(port)
    if state.get("itf") != "4":
        return "expected 4 linked interfaces, got %s" % state.get("itf")
    if state.get("vid") != "1D50" or state.get("pid") != "60FC":
        return "wrong IDs adopted: %s:%s" % (state.get("vid"), state.get("pid"))
    return None


def test_interface_numbering():
    """rawhid must be MI_02, where OnlyKey software looks for it."""
    problem = ensure_application()
    if problem:
        return problem
    nodes = rig.usb_nodes(rig.APP_IDS)
    if not nodes:
        return "no USB nodes for %s" % rig.APP_IDS
    # HID\...&MI_02 is the raw HID interface bound by the HID class driver.
    if not any("MI_02" in n and n.startswith("HID") for n in nodes):
        return ("no HID node at MI_02 -- OnlyKey software locates raw HID by "
                "interface number and will talk to the wrong one. Nodes: %s"
                % nodes)
    return None


def test_counter_reset():
    """`z` zeroes the counters and clears the sticky drop flag."""
    port = rig.find_console()
    if not port:
        return "no proxy console found"
    rig.send(port, "z")
    state = rig.health(port)
    if not state:
        return "no health line after zeroing"
    for field in ("drops", "dropflag", "sent", "indrops"):
        if state.get(field) != "0":
            return "%s did not reset (got %s)" % (field, state.get(field))
    return None


def test_power_cycle():
    """VBUS off detaches the key and leaves a console; on brings it all back."""
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    rig.send(port, "0")  # VBUS off
    time.sleep(3)
    # The proxy re-enumerates console-only, so the port moves. Any console will
    # do here -- what matters is that the board is still reachable with the key
    # unpowered, which is the whole reason console-only mode exists.
    port = rig.wait_for_mode(rig.APP_IDS) or rig.wait_for_mode(rig.BOOT_IDS)
    if not port:
        return "no console after VBUS off -- the board became unreachable"
    state = rig.health(port)
    if not state:
        return "no health line with VBUS off"
    if state.get("itf") != "0":
        return "key still linked with VBUS off (itf=%s)" % state.get("itf")

    rig.send(port, "1")  # VBUS on
    time.sleep(2)
    port = rig.wait_for_mode(rig.APP_IDS, timeout=40)
    if not port:
        # A key that was power-cycled may come back in the bootloader.
        if rig.wait_for_mode(rig.BOOT_IDS, timeout=10):
            problem = ensure_application()
            if problem:
                return problem
            port = rig.find_console(rig.APP_IDS)
        else:
            return "key never came back after VBUS on"
    state = rig.health(port)
    if state.get("itf") != "4":
        return "only %s interfaces after power-on" % state.get("itf")
    return None


def test_bootloader_entry_and_flash():
    """The full unattended cycle: contact -> HalfKay -> flash -> application."""
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    port = rig.enter_bootloader()
    if not port:
        return "key never entered the bootloader after a contact pulse"

    state = rig.health(port)
    if state.get("vid") != "16C0" or state.get("pid") != "0478":
        return "bootloader IDs not adopted: %s:%s" % (state.get("vid"),
                                                      state.get("pid"))
    if state.get("core1") != "ok":
        return "core1=%s in bootloader mode" % state.get("core1")

    code, out = flash()
    if code != 0:
        return "flash failed (exit %d): %s" % (code, out)
    if "0 dropped" not in out:
        return "flasher did not report a clean run: %s" % out

    port = rig.wait_for_mode(rig.APP_IDS, timeout=40)
    if not port:
        return "key did not return to application mode after flashing"
    state = rig.health(port)
    if state.get("dropflag") != "0":
        return "reports were dropped during the flash"
    return None


def test_flash_then_key_responds():
    """A flash is only correct if the key then RUNS what we wrote.

    HalfKay offers no read-back, so the image cannot be verified directly. The
    honest substitute is end-to-end: flash, reboot, and require the firmware to
    boot and answer over raw HID. "210 blocks sent" is exactly the claim that
    was false for so long -- the counters agreed while the key received nothing.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    if not rig.enter_bootloader():
        return "key never entered the bootloader"

    code, out = flash()
    if code != 0:
        return "flash failed (exit %d): %s" % (code, out)

    port = rig.wait_for_mode(rig.APP_IDS, timeout=40)
    if not port:
        return "key did not return to application mode after flashing"

    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "okprobe.py")],
        capture_output=True, text=True)
    if result.returncode != 0:
        return ("key enumerated but its firmware does not answer -- the image "
                "we wrote is not running:\n%s%s" % (result.stdout, result.stderr))
    return None


def test_reboot_only_returns_to_bootloader():
    """A bare reboot from HalfKay must NOT be mistaken for a failure.

    Entering the bootloader invalidates the application firmware, so HalfKay has
    nothing to jump to and comes straight back. This is correct behaviour, and
    pinning it down here stops it being misread as a passthrough bug -- which has
    happened more than once, including as apparent evidence against a fix that
    was working correctly.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    if not rig.enter_bootloader():
        return "key never entered the bootloader"

    code, out = flash(["--boot-only"])
    if code != 0:
        return "reboot failed (exit %d): %s" % (code, out)

    # Expected: back in the bootloader, NOT the application.
    port = rig.wait_for_mode(rig.BOOT_IDS, timeout=40)
    if not port:
        if rig.wait_for_mode(rig.APP_IDS, timeout=5):
            return ("key reached application mode after a bare reboot -- the "
                    "documented quirk no longer holds, so update the docs")
        return "key vanished entirely after a bare reboot"

    problem = ensure_application()  # leave the rig usable
    return problem


def test_recover_from_interrupted_flash():
    """A half-written image must be recoverable without human hands.

    This is the failure an unattended rig will actually hit: a flash dies
    partway and leaves the key with a corrupt application. Recovery must be
    contact-pulse then re-flash, with no button presses.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    port = rig.enter_bootloader()
    if not port:
        return "key never entered the bootloader"

    # Kill a flash partway through, leaving a partial image.
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "halfkay_flash.py"), HEX],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(8)  # past the erase, into the block writes
    proc.kill()
    proc.wait()
    time.sleep(2)

    # Now recover, exactly as an unattended rig would have to.
    port = rig.find_console(rig.BOOT_IDS)
    if not port:
        port = rig.find_console()
        if not port:
            return "no console after an interrupted flash"
        port = rig.enter_bootloader()
        if not port:
            return "could not get back into the bootloader to recover"

    code, out = flash()
    if code != 0:
        return "recovery flash failed (exit %d): %s" % (code, out)
    port = rig.wait_for_mode(rig.APP_IDS, timeout=40)
    if not port:
        return "key did not recover to application mode"
    return None


def test_application_responds():
    """The key's firmware answers over raw HID -- not just enumerates.

    Enumeration only proves the proxy cloned some descriptors. This proves the
    application firmware is running and that raw HID round-trips through the
    passthrough. An "uninitialized" answer is a pass: it is the key's own
    firmware talking.
    """
    problem = ensure_application()
    if problem:
        return problem

    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "okprobe.py")],
        capture_output=True, text=True)
    if result.returncode != 0:
        return "key did not answer over raw HID:\n%s%s" % (result.stdout,
                                                           result.stderr)
    return None


def test_hid_round_trip_integrity():
    """Many raw-HID round trips, with zero losses in either direction.

    Flashing exercises the OUT path with big control transfers; this exercises
    the ordinary report path in both directions. The two use opposite halves of
    the back-pressure rule, so a change can easily fix one and break the other.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)
    rig.send(port, "z")  # count only this test's traffic

    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "okprobe.py"), "--count", "20"],
        capture_output=True, text=True)
    if result.returncode != 0:
        return "round-trip losses:\n%s%s" % (result.stdout, result.stderr)

    state = rig.health(port)
    if not state:
        return "no health line after the round trips"
    if state.get("drops") != "0" or state.get("dropflag") != "0":
        return "proxy dropped host->device reports: %s" % state
    if state.get("indrops") != "0":
        return "proxy dropped device->host reports: indrops=%s" % state.get("indrops")
    if state.get("sent") == "0":
        return "proxy forwarded nothing -- the probe did not reach the key"
    return None


def test_console_garbage_is_harmless():
    """Unrecognised console input must not disturb anything.

    The console is the rig's control channel, so a stray byte from a half-open
    terminal or a mistyped script must be inert. Notably this must NOT hit `R`,
    `U`, `b`, `p` or `0` -- all of which have real side effects -- so only
    characters with no binding are sent.
    """
    port = rig.find_console()
    if not port:
        return "no proxy console found"
    before = rig.health(port)
    if not before:
        return "no health line before the garbage"

    # Deliberately excludes every bound key. Sending 'R' here would reboot the
    # board and 'U' would drop it into BOOTSEL.
    rig.send(port, "qwmnjklyAC@#%^&*()[]{}", wait=1.5)

    after = rig.health(port)
    if not after:
        return "console stopped answering after unrecognised input"
    if after.get("core1") != "ok":
        return "core1=%s after unrecognised input" % after.get("core1")
    if after.get("itf") != before.get("itf"):
        return ("interfaces changed after unrecognised input: %s -> %s"
                % (before.get("itf"), after.get("itf")))
    return None


def test_survive_device_yank_mid_flash():
    """Cutting VBUS mid-flash must not wedge the host stack.

    This is the original bug, provoked deliberately. A device disappearing
    mid-transfer is what desyncs the PIO state machines, and before the bounded
    waits it hung core 1 forever -- after which every host write was ACKed and
    discarded. The rig must instead survive, stay reachable, and be recoverable
    with no hands.

    The flasher runs with --no-health so it does not hold the console: only one
    process may have the port, and this test needs it to cut power.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    port = rig.enter_bootloader()
    if not port:
        return "key never entered the bootloader"

    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "halfkay_flash.py"), HEX,
         "--no-health"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(6)  # past the erase, into the block writes

    rig.send(port, "0")  # yank VBUS out from under the transfer
    time.sleep(3)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        return "the flasher hung when the device was pulled mid-transfer"

    # The board must still be reachable with the key unpowered.
    port = rig.wait_for_mode(rig.APP_IDS, timeout=20) or \
        rig.wait_for_mode(rig.BOOT_IDS, timeout=10)
    if not port:
        return "the proxy became unreachable after a mid-flash device yank"
    state = rig.health(port)
    if not state:
        return "no health line after a mid-flash device yank"
    if state.get("core1") != "ok":
        return ("core1=%s -- the host stack wedged when the device vanished "
                "mid-transfer, which is exactly the bug the bounded PIO waits "
                "are meant to prevent" % state.get("core1"))

    rig.send(port, "1")  # power back on
    time.sleep(3)
    problem = ensure_application()
    if problem:
        return "could not recover after a mid-flash yank: %s" % problem

    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "okprobe.py")],
        capture_output=True, text=True)
    if result.returncode != 0:
        return ("recovered to application mode but the firmware does not "
                "answer:\n%s%s" % (result.stdout, result.stderr))
    return None


def test_watchdog_recovers_from_wedge():
    """A wedged core 1 must reset the board, not sit there being lied about.

    Injected with `!1`, which stops core 1 advancing exactly as a real hang
    inside USBHost.task() does. This is the mechanism that made the original bug
    so expensive: core 0 keeps running and answering the PC perfectly, so every
    host write is ACKed and discarded while a flashing tool reports success.

    Nothing in the ordinary test suite fires this -- 25 clean soak cycles
    exercised it exactly zero times -- so without injection "the tests pass"
    says nothing about whether the watchdog works at all.
    """
    port = rig.find_console()
    if not port:
        return "no proxy console found"

    rig.send(port, "!1", wait=1.0)

    # The board should reset within PROXY_CORE1_WATCHDOG_MS (3 s) and take its
    # USB connection with it.
    if not rig.wait_for_gone(port, timeout=20):
        state = rig.health(port)
        return ("the console never went away -- the watchdog did not fire "
                "(core1=%s)" % (state or {}).get("core1"))

    port = rig.wait_for_mode(rig.APP_IDS, timeout=45) or \
        rig.wait_for_mode(rig.BOOT_IDS, timeout=15)
    if not port:
        return "the board never came back after the watchdog reset it"

    state = rig.health(port)
    if not state:
        return "no health line after the watchdog reset"
    if state.get("core1") != "ok":
        return "core1=%s after the reset -- it did not recover" % state.get("core1")

    problem = ensure_application()
    if problem:
        return "could not recover the key after a watchdog reset: %s" % problem
    return None


def test_dropped_report_is_loud():
    """A discarded report must be counted, stick, and be said out loud.

    Injected with `!2`. A drop is data the PC was already told we delivered, so
    it can never be made good -- the only correct behaviour is to refuse to
    pretend otherwise. The flag is deliberately sticky until `z` or a reset.

    Deliberately does NOT assert that the flasher refuses to start. A flag left
    over from an earlier run must not block a new flash: the flasher zeroes the
    counters at start, which is the documented way to begin a clean run. What
    must abort a flash is a drop DURING it -- see drop_during_flash_aborts.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)
    rig.send(port, "z")

    rig.send(port, "!2", wait=1.0)
    state = rig.health(port)
    if not state:
        return "no health line after injecting a drop"
    if state.get("dropflag") != "1":
        return "dropflag did not set (got %s)" % state.get("dropflag")
    if state.get("drops") == "0":
        return "drops counter did not move"

    # The human-readable block must say so in as many words.
    if "DROPPED" not in rig.status(port):
        return "the status block does not warn about the drop"

    rig.send(port, "z")
    state = rig.health(port)
    if state.get("dropflag") != "0":
        return "`z` did not clear the drop flag"
    return None


def test_drop_during_flash_aborts():
    """A report dropped mid-flash must abort it, not finish and claim success.

    Injected with `!5`, which fires 6 s in. This is the exact failure that made
    the original bug so expensive: a drop is ACKed to the PC before we know we
    cannot forward it, so the flasher is told every write succeeded while the
    key received nothing, and reports a corrupt image as a clean flash.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.enter_bootloader()
    if not port:
        return "key never entered the bootloader"

    rig.send(port, "!5", wait=0.5)  # lands ~6 s into the flash

    code, out = flash(attempts=1)
    if code != 2:
        return ("flasher exited %d despite a report being dropped mid-flash -- "
                "it should abort with 2:\n%s" % (code, out))
    if "DROPPED" not in out.upper():
        return "the flasher did not explain why it aborted:\n%s" % out

    problem = ensure_application()
    if problem:
        return "could not recover after the aborted flash: %s" % problem
    return None


def test_pio_timeout_fails_the_flash():
    """A PIO desync during a flash must invalidate the image.

    Injected with `!3`, which fires 6 s later so it lands mid-flash. A timeout
    means a bounded wait gave up and ABANDONED the transaction, so the block may
    never have reached the key -- while `sent` already counted it. This is why
    "210 sent, 0 dropped" is not proof of a good image, and it is the exact
    condition that produced a key that would not boot.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.enter_bootloader()
    if not port:
        return "key never entered the bootloader"

    rig.send(port, "!3", wait=0.5)  # lands ~6 s into the flash

    code, out = flash(attempts=1)
    if code != 2:
        return ("flasher exited %d despite a PIO desync mid-flash -- it should "
                "refuse the image with 2:\n%s" % (code, out))
    if "desync" not in out:
        return "the flasher did not explain why it refused:\n%s" % out

    problem = ensure_application()
    if problem:
        return "could not recover after the refused flash: %s" % problem
    return None


def test_clone_failure_is_reported():
    """An interface that fails to clone must be counted, not just logged.

    Injected with `!4`. A partial clone is a quiet degradation: the PC simply
    sees fewer interfaces than the key has, and since host software finds raw
    HID by interface NUMBER, a missing one shifts the rest and sends an app to
    the wrong interface. That is indistinguishable from a protocol bug.
    """
    problem = ensure_application()
    if problem:
        return problem
    port = rig.find_console(rig.APP_IDS)

    rig.send(port, "!4", wait=1.0)
    state = rig.health(port)
    if not state:
        return "no health line after injecting a clone failure"
    if state.get("cloneerr", "0") == "0":
        return "cloneerr did not move"

    # Cleared by a detach, since it describes the current attachment.
    rig.send(port, "p", wait=1.0)
    time.sleep(6)
    problem = ensure_application()
    if problem:
        return "could not recover after the power cycle: %s" % problem
    port = rig.find_console(rig.APP_IDS)
    state = rig.health(port)
    if state.get("cloneerr", "0") != "0":
        return ("cloneerr survived a detach (got %s) -- it must describe the "
                "current attachment, not a previous one"
                % state.get("cloneerr"))
    return None


def test_no_silent_drops():
    """After a real flash, nothing may be dropped -- and the flag must be clean."""
    port = rig.find_console()
    if not port:
        return "no proxy console found"
    state = rig.health(port)
    if state.get("dropflag") != "0":
        return ("dropflag is set: reports were discarded after the PC was told "
                "they succeeded")
    if state.get("drops") != "0":
        return "drops=%s" % state.get("drops")
    if state.get("indrops") != "0":
        return "indrops=%s" % state.get("indrops")
    return None


SCENARIOS = [
    ("console_and_health", test_console_and_health),
    ("application_clone", test_application_clone),
    ("interface_numbering", test_interface_numbering),
    ("counter_reset", test_counter_reset),
    ("application_responds", test_application_responds),
    ("hid_round_trip_integrity", test_hid_round_trip_integrity),
    ("bootloader_entry_and_flash", test_bootloader_entry_and_flash),
    ("flash_then_key_responds", test_flash_then_key_responds),
    ("reboot_only_returns_to_bootloader", test_reboot_only_returns_to_bootloader),
    ("recover_from_interrupted_flash", test_recover_from_interrupted_flash),
    ("survive_device_yank_mid_flash", test_survive_device_yank_mid_flash),
    ("console_garbage_is_harmless", test_console_garbage_is_harmless),
    ("power_cycle", test_power_cycle),
    # Fault injection: proves each safety mechanism actually fires. Kept last
    # because they deliberately break things, and a failure here is easier to
    # read once the ordinary paths are known good.
    ("watchdog_recovers_from_wedge", test_watchdog_recovers_from_wedge),
    ("dropped_report_is_loud", test_dropped_report_is_loud),
    ("drop_during_flash_aborts", test_drop_during_flash_aborts),
    ("pio_timeout_fails_the_flash", test_pio_timeout_fails_the_flash),
    ("clone_failure_is_reported", test_clone_failure_is_reported),
    ("no_silent_drops", test_no_silent_drops),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-k", dest="filter", help="only scenarios matching this")
    args = ap.parse_args()

    if args.list:
        for name, func in SCENARIOS:
            summary = (func.__doc__ or "").strip().splitlines()[0]
            print("  %-36s %s" % (name, summary))
        return 0

    selected = [(n, f) for n, f in SCENARIOS
                if not args.filter or args.filter in n]
    if not selected:
        print("no scenarios match %r" % args.filter)
        return 1

    print("running %d scenario(s)\n" % len(selected))
    started = time.time()
    for name, func in selected:
        sys.stdout.write("  %-36s " % name)
        sys.stdout.flush()
        began = time.time()
        try:
            problem = func()
        except Exception as exc:
            problem = "raised %s: %s" % (type(exc).__name__, exc)
        if problem:
            print("FAIL  (%.0fs)\n" % (time.time() - began))
            print("    %s\n" % problem)
            port = rig.find_console()
            if port:
                print(rig.status(port))
            return 1
        print("ok    (%.0fs)" % (time.time() - began))

    print("\nall %d scenarios passed in %.0f s"
          % (len(selected), time.time() - started))
    port = rig.find_console()
    if port:
        print("final: %s" % rig.health(port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
