# Known-working workflows

Procedures that have been run and verified on this rig. Every command here has
been executed successfully; nothing is aspirational.

Paths assume this machine: a Linux box with a system Python 3 that has pyserial,
and PlatformIO installed under `~/.platformio`. `pio` is not on PATH.

```bash
PIO=~/.platformio/penv/bin/platformio
BASE=~/projects/onlykey/onlykey-usb-hid-passthrough
```

Everything else is stdlib, deliberately: `tools/linuxhid.py` talks to hidraw
through `os` and `select`, so there is no `hidapi` to install.

Three rules that apply to everything below:

- **Only one process may hold the console.** A second opener loses the capture.
  Stop any logger before running a flash — the flasher opens the console itself
  for health checks.
- **The console's device node can move** between `/dev/ttyACM0` and
  `/dev/ttyACM1` as the key changes mode, because the proxy adopts the key's
  VID/PID and re-enumerates. Look it up every time; every tool here does.
- **Permissions come from udev, not from group membership.** You do not need to
  be in `dialout`.

---

## Permissions

Three USB identities pass through this rig, and each needs a rule:

| IDs | What | Rule file |
|---|---|---|
| `1D50:60FC` | OnlyKey application | `/etc/udev/rules.d/49-onlykey.rules` |
| `16C0:0478` | Teensy HalfKay | `/etc/udev/rules.d/00-teensy.rules` |
| `2E8A:*` | RP2040 BOOTSEL | `tools/60-rp2040-bootsel.rules` |

The first two ship with OnlyKey and Teensy. The third is in this repo because
nothing else installs it:

```bash
sudo cp $BASE/tools/60-rp2040-bootsel.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Without it `picotool` finds the board and then fails to claim it, which looks
like the board never reached BOOTSEL rather than like a permission problem.

The OnlyKey rule has no `hidraw` line, but its `SUBSYSTEMS=="usb" … MODE:="0666"`
propagates to the hidraw children, so all four interfaces come out `0666`.

## Build

```bash
$PIO run -d $BASE                                     # all three environments
$PIO run -d $BASE -e adafruit_feather_rp2040_usb_host  # the real one
```

`-d $BASE` matters: the working directory is not always the project root. A full
three-environment build takes about 9 s.

## Flash the Feather — no button press

```bash
python3 $BASE/tools/touch1200.py /dev/ttyACM0
sleep 5
$PIO run -d $BASE -e adafruit_feather_rp2040_usb_host -t upload
```

The 1200-baud DTR touch enters BOOTSEL; `upload_command` runs
`picotool load -x`, which loads the UF2 and runs it. Nothing needs the `RPI-RP2`
drive mounted — this rig does not automount it.

Verified: touch → `2E8A:0003` on the bus → `picotool` writes and reboots in
10.2 s → the proxy returns with all four interfaces and `piotimeouts=0`, an
RP2040 reset having reinitialised the USB library's own counter.

If the board is reset-looping and the console keeps vanishing, retry the touch in
a loop until `rig.in_bootsel()` is true; a window always comes up within a few
tries.

## Talk to the proxy

```bash
python3 $BASE/tools/console.py                # auto-detect, print health
python3 $BASE/tools/console.py --keys i       # human-readable status
python3 $BASE/tools/console.py --keys z       # zero counters before a measurement
python3 $BASE/tools/console.py --watch 60     # log for 60 s
```

`s` (health) is the machine-readable line scripts gate on. `i` is for people.

## Inspect the cloned interfaces

```bash
python3 $BASE/tools/linuxhid.py               # application mode, 1D50:60FC
python3 $BASE/tools/linuxhid.py 16C0 0478     # HalfKay
```

Reports each interface's path, USB interface number, usage page, report sizes,
and **which wire path an output report will take**. Verified in application mode:

```
/dev/hidraw0  mi=0 usage_page=0x0001 usage=0x0006 in=8  out=1   control SET_REPORT
/dev/hidraw1  mi=1 usage_page=0xF1D0 usage=0x0001 in=64 out=64  interrupt OUT
/dev/hidraw2  mi=2 usage_page=0xFFAB usage=0x0002 in=64 out=64  interrupt OUT
/dev/hidraw3  mi=3 usage_page=0xFFC9 usage=0x0004 in=64 out=32  interrupt OUT
```

That agrees with the proxy's own `i` block, which reports `OUT ep no` for slot 0
and `OUT ep yes` for the other three — a useful cross-check that the clone is
right, from the other side of the link.

## Put the key into its bootloader

```bash
python3 $BASE/tools/console.py --keys b       # press/release the contact
sleep 6
```

Works on a key that is **running firmware**. Lands at `16C0:0478` with a HID
interface bound, typically in under 2 s. Grounding the physical contact by hand
does the same thing.

Measured on HalfKay: one interface, one endpoint — `ep_81`, interrupt IN, 64
bytes. There is **no OUT endpoint**, which is why a 1088-byte firmware block
goes out as a control-pipe SET_REPORT without anything having to ask for it.

## Flash the key

```bash
cd $BASE && python3 tools/halfkay_flash.py firmware.hex
```

Finds the proxy console itself, zeroes counters, refuses to start if `core1` is
not `ok`, checks every 16 blocks and aborts at the failure.

Exit `0` success, `1` flash error, `2` proxy fault (data lost — **re-flash**,
the image cannot be trusted).

A good run, verified:

```
found 16C0:0478 at /dev/hidraw0  usage_page=0xFF9C usage=0x0021  output report=1088 bytes
proxy console on /dev/ttyACM0 -- will abort on any dropped report
read firmware.hex: 214400 bytes (81.8% of 262144)
programmed 210 blocks (215040 bytes) in 9.94 s
proxy: 210 sent, 0 dropped
```

Afterwards the key returns to application mode automatically. Confirm the
firmware actually runs:

```bash
cd $BASE/tools && python3 okprobe.py
```

An `Error OnlyKey must be initialized first` reply is a **pass** — that is the
key's own firmware answering.

## Cold power cycle

```bash
python3 $BASE/tools/console.py --keys p        # VBUS cycle
```

A genuine cold start: the key runs its LED boot sequence and emits a fresh
startup banner on seremu. Lands in **application** mode when the firmware is
valid.

For a power cycle that does not depend on the proxy firmware running at all:

```bash
cd $BASE/tools && python3 coldcycle.py --hold 6
```

Routes through the RP2040 ROM bootloader, where every pad is at reset state and
the key is provably off the bus. Use this when the proxy itself is wedged. Needs
the `2E8A` udev rule. A verified run, 14 s end to end:

```
console: /dev/ttyACM0
BOOTSEL (2E8A:0003) -- the key is now unpowered
key nodes on the bus while unpowered: 0
holding 6.0 s
UF2 loaded; the proxy is restarting
key is back: 1D50:60FC, itf=4, core1=ok
```

`key nodes on the bus while unpowered: 0` is the point of the tool — it is the
proof, not an assumption, that the key is genuinely unpowered.

It lands in **application** mode here because the firmware is valid. The tool
waits for either mode rather than assuming the bootloader; assuming it cost a
45 s timeout on every ordinary run.

## Recover a key that has gone silent

In order of escalation. The proxy does this automatically, but by hand:

1. `--keys b` — press/release. Works if the key is running firmware.
2. `--keys B` — cold entry: VBUS cycle, let it boot, press, release.
3. `--keys R` — RP2040 reset. **The recovery measured to work every time** on
   a key stuck descriptor-less (`itf=0`).

A descriptor-less key looks healthy from outside — console present, right
VID/PID, `core1=ok` — and only `itf=0` reveals it. After recovery it will be in
the bootloader, so flash it to get back to application mode.

`--keys A` switches automatic recovery off, which is necessary before poking at
the board by hand — otherwise it power-cycles every few seconds and takes the
console with it.

## Test

```bash
cd $BASE/tools
python3 selftest.py            # 19 scenarios, fail-fast
python3 selftest.py --list     # what they are
python3 selftest.py -k flash   # only matching
python3 soak.py 25             # 25 bootloader<->application cycles
python3 okprobe.py --count 20  # raw-HID round trips only
```

`selftest.py` stops at the first failure and prints the `i` block. Scenarios that
enter the bootloader flash back, so the rig is left usable.

Run `selftest.py` after any change to the OUT path, the recovery code or the
vendored USB libraries.

## Capture what the PC sends

For diagnosing a flashing tool that is not `halfkay_flash.py`:

```bash
python3 $BASE/tools/console.py --keys z       # zero first
python3 $BASE/tools/console.py --watch 420    # as the ONLY process on the port
```

Each forwarded report logs as
`[out] slot N id=.. type=.. len=.. : XX XX XX XX`. For a HalfKay block the first
three bytes are the flash address. A dropped report logs as `[out] DROPPED ...`
with the same detail, which identifies exactly which block was lost.

## Fault injection

Proves the safety mechanisms fire. Two keys, so a stray byte cannot trigger one.

```bash
python3 $BASE/tools/console.py --keys '!1'    # wedge core 1 -> watchdog resets
python3 $BASE/tools/console.py --keys '!2'    # drop a report -> sticky flag, LED
python3 $BASE/tools/console.py --keys '!3'    # PIO timeout in 6 s -> mid-flash
python3 $BASE/tools/console.py --keys '!4'    # clone failure -> cloneerr
python3 $BASE/tools/console.py --keys '!5'    # drop in 6 s -> aborts a flash
```

Quote the argument: `!` is history expansion in an interactive bash shell.

`!3` and `!5` are deferred because the flasher holds the console while running,
so there is no way to inject from outside during a flash.

## Verified end-to-end sequence

The full unattended chain, all steps confirmed working together on Linux:

1. Ground the contact (by hand or `b`) — key leaves application mode
2. If it lands descriptor-less, the proxy recovers it to the bootloader by
   itself: cold entries, backoff, then an RP2040 reset
3. Flash with `halfkay_flash.py` — 210 blocks, zero dropped
4. Reboot — key returns as `1D50:60FC` with all four interfaces
5. `okprobe.py` — firmware answers over raw HID
