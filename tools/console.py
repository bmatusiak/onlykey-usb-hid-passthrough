"""Talk to the Feather proxy's console: send commands, log what comes back.

Exists so there is ONE checked-in way to hold the port. Only one process may
have it open -- a second opener gets PermissionError(13) and whatever you were
capturing is lost -- so use this rather than improvising, and do not run it
during a flash (halfkay_flash.py opens the console itself for health checks).

    python console.py                    find the port, print health, exit
    python console.py --keys i           send 'i', show the reply
    python console.py --keys b           put the KEY into its bootloader
    python console.py --watch 60         log for 60 s (verbose OUT stays as-is)
    python console.py --port COM15 ...   skip auto-detection

The port number MOVES when the key changes mode, because the proxy adopts the
key's VID/PID and re-enumerates: application mode is 1D50:60FC, bootloader mode
is 16C0:0478, and Windows hands out a different COM for each. Auto-detection is
the default for exactly that reason.
"""
import argparse
import sys
import time

import serial

import rig

# The proxy clones the key, so its console carries the key's IDs.
KNOWN = ("1D50:60FC", "16C0:0478")


def find_port():
    """Return the proxy console, preferring a port whose IDs we recognise.

    Enumeration goes through rig.comports(), which survives a device vanishing
    mid-scan -- pyserial's Linux backend raises TypeError when that happens.
    """
    ports = rig.comports()
    for port in ports:
        hwid = (port.hwid or "").upper()
        if any(("VID:PID=" + known) in hwid for known in KNOWN):
            return port.device
    # Fall back to a lone serial port rather than failing outright; a build with
    # a different VID/PID is still worth talking to.
    if len(ports) == 1:
        return ports[0].device
    return None


def run(port, keys, watch):
    ser = serial.Serial(port, 115200, timeout=0.05)
    started = time.time()

    for key in keys:
        ser.write(key.encode())
        ser.flush()
        time.sleep(0.3)

    # Always drain a little, so a bare --keys still shows its reply.
    deadline = started + (watch if watch else 1.5)
    buf = b""
    try:
        while time.time() < deadline:
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    print("%7.3f  %s" % (time.time() - started, text))
                    sys.stdout.flush()
    except Exception as exc:
        # Entering or leaving the key's bootloader re-enumerates the proxy and
        # takes this port with it. That is a normal end to a capture.
        print("%7.3f  === port went away: %s ===" % (time.time() - started, exc))

    try:
        ser.close()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="console port; auto-detected if omitted")
    ap.add_argument("--keys", default="", help="console keys to send, e.g. 'i'")
    ap.add_argument("--watch", type=float, default=0,
                    help="seconds to keep logging after sending")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("no proxy console found -- is the Feather plugged in?")
        return 1
    print("console: %s" % port)

    # With nothing asked for, the useful default is the health line.
    run(port, args.keys or "s", args.watch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
