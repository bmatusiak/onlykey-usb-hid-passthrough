"""Ask the OnlyKey a question through the proxy and check that it answers.

Enumeration only proves the proxy cloned some descriptors. It says nothing about
whether the key's application firmware is running or whether raw HID actually
round-trips. This sends a real OnlyKey message and reads the real reply.

An uninitialized key answers with UNINITIALIZED / "must be initialized first",
which is a perfectly good result here: it is the key's own firmware talking, so
the whole TX and RX path through the proxy is proven end to end.

    python okprobe.py           probe and print what the key said
    python okprobe.py --count 20  round-trip N times and report any losses

Exit 0 if the key answered, 1 if it did not.
"""
import argparse
import sys
import time

import linuxhid

VID, PID = 0x1D50, 0x60FC

# OnlyKey messages are 64-byte reports: a four-byte header, then a message type.
OK_HEADER = bytes([255, 255, 255, 255])
OKGETLABELS = 0xE5

# The raw HID interface. On a real OnlyKey this is MI_02, and the proxy clones
# interface order deliberately so that stays true through the passthrough.
RAWHID_MI = 2


def message(msg_type, payload=b""):
    return OK_HEADER + bytes([msg_type]) + bytes(payload)


def printable(reports):
    """Pull readable text out of raw input reports."""
    text = []
    for report in reports:
        # The whole report is payload: the OnlyKey declares no report IDs, and
        # a hidraw read does not prepend one (the Windows path did, which is why
        # this used to skip byte 0 -- skipping here would eat a data byte). The
        # key pads with NULs and 0xFF.
        chars = [chr(b) for b in report
                 if 32 <= b < 127]
        chunk = "".join(chars).strip()
        if chunk:
            text.append(chunk)
    return text


def probe(device, quiet=False):
    """Send one query, return the decoded reply lines."""
    device.drain(0.3)  # clear anything already queued
    if not device.write(message(OKGETLABELS)):
        return None
    replies = device.drain(2.0)
    lines = printable(replies)
    if not quiet:
        for line in lines:
            print("    key says: %s" % line)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=1,
                    help="round-trip this many times and check for losses")
    ap.add_argument("--settle", type=float, default=20.0,
                    help="seconds to keep retrying the first query")
    args = ap.parse_args()

    # Enumerating is not the same as being ready. Straight after a flash the key
    # is still running its startup -- seremu is visibly streaming its banner --
    # and raw HID does not answer for several seconds. Probing the instant the
    # device appears therefore reports a dead key that is merely busy, which it
    # did on the first run of this test. Retry until it answers or the settle
    # window expires.
    device = None
    deadline = time.time() + args.settle
    while time.time() < deadline:
        device = linuxhid.open_interface(VID, PID, mi=RAWHID_MI)
        if device:
            break
        time.sleep(0.5)
    if not device:
        print("no raw HID interface at MI_%02d for %04X:%04X -- is the key in "
              "application mode?" % (RAWHID_MI, VID, PID))
        return 1

    with device:
        print("raw HID: usage_page=0x%04X usage=0x%02X in=%d out=%d"
              % (device.caps.UsagePage, device.caps.Usage,
                 device.caps.InputReportByteLength,
                 device.caps.OutputReportByteLength))

        # First reply: keep asking until the key is ready. Only once it has
        # answered at all does a non-answer mean something is wrong.
        saw = []
        first = None
        while time.time() < deadline:
            first = probe(device, quiet=True)
            if first:
                break
            time.sleep(0.75)
        if not first:
            print("no reply within %.0fs of settling" % args.settle)
            return 1
        waited = args.settle - (deadline - time.time())
        if waited > 1.0:
            print("(key took %.1fs to start answering raw HID)" % waited)
        for line in first:
            print("    key says: %s" % line)

        answered = 1
        saw.extend(first)
        started = time.time()
        for attempt in range(args.count - 1):
            lines = probe(device, quiet=True)
            if lines:
                answered += 1
                saw.extend(lines)

        elapsed = time.time() - started
        print("\n%d/%d round-trips answered in %.1f s"
              % (answered, args.count, elapsed))

        if not answered:
            print("the key never replied -- either the application firmware is "
                  "not running, or raw HID is not round-tripping through the "
                  "proxy")
            return 1

        joined = " ".join(saw).upper()
        if "UNINITIALIZED" in joined or "INITIALIZED" in joined:
            print("firmware is alive and talking")
        if answered < args.count:
            print("WARNING: %d round-trip(s) got no reply"
                  % (args.count - answered))
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
