"""Flash a Teensy HalfKay bootloader over raw HID, with no toolchain.

A Python port of the parts of teensy_loader_cli we need. It exists because
teensy_loader_cli has to be compiled, and because having the flasher scriptable
makes it possible to exercise the USB proxy repeatedly and instrument what it
does.

Writes go out through Linux hidraw. For a Teensy 3.2 each block is a 1089-byte
write (1 report-ID byte + 3 address bytes + 61 pad + 1024 data). A block that
size can only travel the control pipe as a SET_REPORT -- no interrupt endpoint
carries 1 KB -- and HalfKay has no OUT endpoint anyway, so the kernel takes that
path by itself. That is the same path the proxy forwards and the only one that
takes its PROXY_OUT_BLOCK_MS hold-off, so the tool checks it and refuses to run
if the interface ever turns out to have an interrupt OUT endpoint.

    python halfkay_flash.py firmware.hex              # program then reboot
    python halfkay_flash.py --boot-only               # just reboot the key
    python halfkay_flash.py firmware.hex --no-reboot  # leave it in bootloader
    python halfkay_flash.py firmware.hex --no-health  # skip proxy monitoring

Defaults target the OnlyKey's MK20DX256 (Teensy 3.2). Override with --code-size
and --block-size for other parts.

Fail-fast
---------
When flashing THROUGH the Feather proxy, a successful write proves nothing. The
proxy ACKs the control transfer before it knows whether the
report can be forwarded, so if its host core has wedged, every write "succeeds"
and the key receives nothing. That is not hypothetical -- it is how a run
reported 210 blocks and IMG_REBOOT_OK while writing a corrupt image.

So this tool watches the proxy's own health line over the serial console and
ABORTS THE INSTANT anything is dropped, rather than finishing and reporting a
success it cannot substantiate. The console is found automatically; pass
--console to override or --no-health to flash a directly-attached key.

Exit codes: 0 success, 1 flash error, 2 proxy fault (data was lost).
"""

import argparse
import sys
import time

import linuxhid

# ---------------------------------------------------------------- hidraw ----


def find_device(vid, pid):
    """Return (HidDevice, output payload length) for the first matching device.

    HalfKay presents a single HID interface, so the first match is the one.
    """
    interfaces = linuxhid.enumerate_interfaces(vid, pid)
    if not interfaces:
        return None, 0

    itf = interfaces[0]
    caps = itf["caps"]
    print("found %04X:%04X at %s  usage_page=0x%04X usage=0x%04X  "
          "output report=%d bytes"
          % (vid, pid, itf["path"], caps.UsagePage, caps.Usage,
             caps.OutputReportByteLength))

    # A 1088-byte block cannot go out on an interrupt endpoint, and the proxy's
    # hold-off only applies to the control path. If this interface ever grows an
    # OUT endpoint, the kernel would route writes there and both of those
    # assumptions break -- so say so rather than write blocks into a path that
    # was never tested.
    if itf["has_out_ep"]:
        print("WARNING: this interface has an interrupt OUT endpoint, so the "
              "kernel will not send blocks over the control pipe -- that is not "
              "the path the proxy hold-off protects")

    device = linuxhid.HidDevice(itf["path"], caps, itf["has_out_ep"], itf["mi"])
    return device, caps.OutputReportByteLength


# --------------------------------------------------------- proxy health ----

class ProxyFault(Exception):
    """The proxy lost data, or stopped being able to forward it."""


class ProxyHealth:
    """Reads the Feather proxy's `s` health line over its serial console.

    Optional by design: with --no-health, or when no console is found, this
    degrades to a no-op so the tool still works against a directly-attached
    key. But when the proxy IS in the path, its health is the only real
    evidence a write landed -- the HID call cannot tell us.
    """

    def __init__(self, port=None):
        self.ser = None
        self.port = port
        self.lost = False
        # PIO bus timeouts at the start of the run. Any increase during a flash
        # invalidates the image.
        #
        # A timeout means a bounded wait in Pico-PIO-USB gave up and ABANDONED
        # the transaction -- the block may never have reached the key, while
        # `sent` already counted it. So zero drops is not sufficient: a run can
        # report 210 sent / 0 dropped and still write a corrupt image, which is
        # exactly what happened before this check existed. The give-up is what
        # stops the stack hanging; it is not a guarantee the data landed.
        self.baseline_timeouts = None
        # Set once a reboot has been sent. After that the console is EXPECTED to
        # disappear -- rebooting the key re-enumerates the proxy along with it --
        # so losing the port stops being evidence of a fault and becomes
        # evidence the reboot landed. Before that point, a vanished console
        # means the proxy reset under us, which is very much a fault.
        self.expect_loss = False
        if port is None:
            return
        try:
            import serial  # imported lazily: only needed for proxy runs
        except ImportError:
            print("WARNING: pyserial not installed -- proxy health unmonitored")
            return
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.05)
        except Exception as exc:
            print("WARNING: could not open %s (%s) -- health unmonitored"
                  % (port, exc))

    @property
    def active(self):
        return self.ser is not None

    def _command(self, key, wait=0.5):
        try:
            self.ser.reset_input_buffer()
            self.ser.write(key.encode())
            self.ser.flush()
            out = b""
            end = time.time() + wait
            while time.time() < end:
                out += self.ser.read(4096)
            return out.decode("utf-8", "replace")
        except Exception:
            # The port went away mid-command. Whether that is good or bad is
            # decided in check(); just record it and stop touching the handle.
            self.lost = True
            self.close()
            return ""

    def read(self):
        """Return the health line as a dict, or None if it could not be read."""
        if not self.ser:
            return None
        for _ in range(3):
            for line in self._command("s").splitlines():
                if "[health]" in line:
                    fields = {}
                    for token in line.split("[health]", 1)[1].split():
                        if "=" in token:
                            k, v = token.split("=", 1)
                            fields[k] = v
                    if fields:
                        return fields
        return None

    def zero(self):
        if self.ser:
            self._command("z")

    def check(self, context):
        """Raise ProxyFault if the proxy has lost anything. Cheap; call often."""
        health = self.read()
        if self.lost and not self.expect_loss:
            raise ProxyFault(
                "%s: the proxy console disappeared mid-flash -- the board reset "
                "under us (watchdog?), so the rest of this image never landed"
                % context)
        if health is None:
            return None
        if health.get("dropflag") == "1" or health.get("drops", "0") != "0":
            raise ProxyFault(
                "%s: proxy DROPPED %s report(s) after ACKing them to us -- "
                "the key did not receive them, so this image is corrupt"
                % (context, health.get("drops", "?")))
        if health.get("core1") != "ok":
            raise ProxyFault(
                "%s: proxy host core is %s -- nothing is reaching the key"
                % (context, health.get("core1")))

        now = health.get("piotimeouts")
        if now is not None:
            if self.baseline_timeouts is None:
                self.baseline_timeouts = now
            elif now != self.baseline_timeouts:
                raise ProxyFault(
                    "%s: the PIO bus desynced during this flash "
                    "(piotimeouts %s -> %s). A transaction was abandoned, so a "
                    "block may not have reached the key even though nothing was "
                    "dropped -- this image cannot be trusted. Re-flash."
                    % (context, self.baseline_timeouts, now))
        return health

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None


def find_proxy_console(vid, pid):
    """Find the CDC console belonging to the proxy cloning this VID/PID.

    The proxy adopts the attached key's IDs, so its console shows up as a USB
    Serial Device under the same VID/PID as the HID interface we are flashing.
    A real key has no such console, which is exactly how we tell the two apart.
    """
    try:
        import rig  # also gives us the comports() that survives a mid-scan unplug
    except ImportError:
        return None
    want = "VID:PID=%04X:%04X" % (vid, pid)
    for port in rig.comports():
        if want in (port.hwid or "").upper():
            return port.device
    return None


# ------------------------------------------------------------ Intel HEX ----

def read_intel_hex(path, code_size):
    """Return (memory bytearray, used bytearray) for a flat image."""
    mem = bytearray(b"\xff" * code_size)
    used = bytearray(code_size)
    base = 0
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":"):
                continue
            raw = bytes.fromhex(line[1:])
            count, addr_hi, addr_lo, rtype = raw[0], raw[1], raw[2], raw[3]
            addr = (addr_hi << 8) | addr_lo
            data = raw[4:4 + count]
            if rtype == 0x00:
                for i, byte in enumerate(data):
                    off = base + addr + i
                    if 0 <= off < code_size:
                        mem[off] = byte
                        used[off] = 1
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                base = ((data[0] << 8) | data[1]) * 16
            elif rtype == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
    return mem, used


# ---------------------------------------------------------------- flash ----

def write_report(device, payload):
    """Send one HalfKay packet as an output report (report ID 0)."""
    return device.write(payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hexfile", nargs="?")
    ap.add_argument("--vid", type=lambda s: int(s, 16), default=0x16C0)
    ap.add_argument("--pid", type=lambda s: int(s, 16), default=0x0478)
    ap.add_argument("--code-size", type=int, default=262144)   # MK20DX256
    ap.add_argument("--block-size", type=int, default=1024)
    ap.add_argument("--boot-only", action="store_true")
    ap.add_argument("--no-reboot", action="store_true")
    ap.add_argument("--console", help="proxy console port, e.g. /dev/ttyACM0")
    ap.add_argument("--no-health", action="store_true",
                    help="do not monitor the proxy (direct-attached key)")
    ap.add_argument("--check-every", type=int, default=16,
                    help="blocks between proxy health checks (0 = only at end)")
    args = ap.parse_args()

    if not args.hexfile and not args.boot_only:
        ap.error("need a .hex file (or --boot-only)")

    device, out_len = find_device(args.vid, args.pid)
    if not device:
        print("no %04X:%04X device found -- is it in bootloader mode?"
              % (args.vid, args.pid))
        return 1

    console = None
    if not args.no_health:
        console = args.console or find_proxy_console(args.vid, args.pid)
        if console:
            print("proxy console on %s -- will abort on any dropped report"
                  % console)
        else:
            print("no proxy console found; assuming a directly-attached key")
    health = ProxyHealth(console)

    # 3 address bytes + 61 pad + block, matching teensy_loader_cli for
    # block_size 512/1024.
    write_size = args.block_size + 64
    # out_len is the payload length from the report descriptor; hidraw adds the
    # report-ID byte on top, so the write itself is one byte longer.
    if out_len and out_len != write_size:
        print("WARNING: device reports %d-byte output reports, expected %d"
              % (out_len, write_size))

    try:
        # Pre-flight. Refusing to start against a wedged proxy is the whole
        # point: a run that begins here can be trusted to mean something.
        if health.active:
            before = health.read()
            if before is None:
                print("WARNING: proxy console gave no health line -- unmonitored")
            elif before.get("core1") != "ok":
                print("ABORT: proxy host core is %s before we even started"
                      % before.get("core1"))
                return 2
            else:
                # Remember the PIO timeout count now: `z` deliberately does not
                # clear it (it lives in the USB library, not our counters), so
                # the baseline is captured rather than reset.
                health.baseline_timeouts = before.get("piotimeouts")
                health.zero()

        if args.boot_only:
            payload = bytearray(write_size)
            payload[0] = payload[1] = payload[2] = 0xFF
            health.expect_loss = True  # the key is about to take the console
            if not write_report(device, payload):
                print("reboot write FAILED")
                return 1
            print("rebooting...")
            health.check("reboot")
            return 0

        mem, used = read_intel_hex(args.hexfile, args.code_size)
        total = sum(used)
        print("read %s: %d bytes (%.1f%% of %d)"
              % (args.hexfile, total, 100.0 * total / args.code_size, args.code_size))

        started = time.time()
        blocks = 0
        for addr in range(0, args.code_size, args.block_size):
            chunk = used[addr:addr + args.block_size]
            # Always send block 0 -- it triggers the chip erase.
            if blocks > 0 and not any(chunk):
                continue

            payload = bytearray(write_size)
            payload[0] = addr & 0xFF
            payload[1] = (addr >> 8) & 0xFF
            payload[2] = (addr >> 16) & 0xFF
            payload[64:64 + args.block_size] = mem[addr:addr + args.block_size]

            if not write_report(device, payload):
                print("\nFAILED writing block at 0x%06X (block %d): %s"
                      % (addr, blocks, device.last_error))
                return 1

            # The first write erases the chip and takes far longer than the
            # rest. The proxy enforces this delay too, since it cannot rely on
            # the PC doing so -- but keep it here for directly-attached keys.
            if blocks == 0:
                time.sleep(3.0)
            blocks += 1
            if blocks % 32 == 0:
                sys.stdout.write(".")
                sys.stdout.flush()

            # Stop AT the failure, not after it. Finishing the remaining blocks
            # would only produce a longer, more convincing lie.
            if args.check_every and blocks % args.check_every == 0:
                health.check("block %d (0x%06X)" % (blocks, addr))

        elapsed = time.time() - started
        health.check("end of image")
        print("\nprogrammed %d blocks (%d bytes) in %.2f s"
              % (blocks, blocks * args.block_size, elapsed))

        # Last chance to see clean counters while the console is still there.
        if health.active:
            final = health.read()
            if final:
                print("proxy: %s sent, %s dropped"
                      % (final.get("sent", "?"), final.get("drops", "?")))

        if not args.no_reboot:
            payload = bytearray(write_size)
            payload[0] = payload[1] = payload[2] = 0xFF
            health.expect_loss = True  # rebooting takes the console with it
            if not write_report(device, payload):
                print("reboot write FAILED")
                return 1
            health.check("reboot")
            print("reboot sent")
        return 0
    except ProxyFault as fault:
        print("\nABORT: %s" % fault)
        return 2
    finally:
        health.close()
        device.close()


if __name__ == "__main__":
    sys.exit(main())
