# OnlyKey USB HID passthrough

A USB HID proxy for the **Adafruit Feather RP2040 with USB Type-A Host**. An
OnlyKey plugs into the Feather's Type-A port; the Feather presents itself to the
PC as that same key and forwards every HID report in both directions —
**including when the key is in its firmware bootloader**.

The point of the exercise is the **power pin**: GPIO 18 gates 5 V to the Type-A
port, so the key can be powered down and back up under software control, and the
PC sees a genuine unplug/replug rather than a device that quietly stops
responding.

```
   PC  <=====[ native USB ]=====  Feather RP2040  =====[ Type-A host ]=====>  OnlyKey
                device stack                                 host stack
                  (core 0)                                    (core 1)
```

Descriptors are **cloned at runtime**, not compiled in, so the proxy forwards
whatever enumerates — application firmware or bootloader — and adopts the
attached device's VID/PID so flashing tools recognise it.

---

## Hardware

**Adafruit Feather RP2040 with USB Type A Host** — <https://www.adafruit.com/product/5723>

Both connectors on the board are female, so the whole setup takes **one cable**:
one USB cable from the PC to the Feather, and the OnlyKey's male Type-A plug
straight into the board's Type-A socket. No adapter, no second cable.

The board must be the USB-Host variant. A plain Feather RP2040 has no Type-A
connector and no 5 V load switch.

| GPIO | Role |
|---|---|
| 16 / 17 | USB host D+ / D− (bit-banged by PIO) |
| 18 | 5 V enable for the Type-A port — the power control |
| 13 | onboard LED (board state) |
| 21 / 20 | NeoPixel / NeoPixel power |
| 7 | BOOT button (plain GPIO at runtime) |
| 6 | OnlyKey bootloader contact — **optional**, see below |
| 0 / 1 | UART TX / RX — only used as the console when `PROXY_ENABLE_CDC=0` |

### Optional: automating the bootloader contact

The OnlyKey enters its bootloader when a contact point is pulled to ground.
Wiring that to GPIO 6 lets the firmware trigger it — no button press, and it
enables automatic recovery (below).

**Use a transistor, not a direct wire.** RP2040 pads come out of reset as inputs
with a **pull-down**, before any firmware runs. Wired directly, that pull-down
sits on the key's button line during every reset and flash and can read as a
stuck press. With a transistor the pull-down lands on the gate instead, holding
it *off* — the hazard becomes the safe default.

```
Feather GPIO 6 ──────┬──── Gate (2N7002 / BSS138)
                     │
                   [100k]        Drain ──── OnlyKey bootloader contact
                     │
                    GND ──────── Source ─── GND (shared via USB)
```

An NPN works identically: GPIO 6 → 1 kΩ → base, 10 kΩ base-to-GND, emitter to
GND, collector to the contact. Ground is already common through the Type-A
connector, so this is one wire plus the part.

This also keeps the Feather's pin off the OnlyKey's button line entirely, so
nothing can back-feed the key while it is unpowered.

Set `PROXY_BOOTSEL_ACTIVE_HIGH` to `0` for a direct connection (the **default**,
open-drain, never driven high) or `1` once a transistor is fitted (drive high).

**This is not a harmless preference.** With a direct wire and `ACTIVE_HIGH=1`,
"released" drives the pin *low*, which grounds the key's bootloader contact
permanently and stops it booting at all — the key goes completely dark and looks
broken. The default matches a direct wire so a plain `pio run` is always safe;
only opt into `1` when the transistor is physically present.

---

## What the PC sees

### Application mode

| Interface | Windows name | Purpose |
|---|---|---|
| MI_00 | HID Keyboard Device | keyboard |
| MI_01 | HID-compliant fido | FIDO2 / U2F (CTAPHID) |
| MI_02 | HID-compliant vendor-defined | OnlyKey app raw HID |
| MI_03 | HID-compliant vendor-defined | Teensy seremu debug |
| MI_04 | USB Serial Device (COM*n*) | control console |

**The HID interfaces come first on purpose**, so they land on the same interface
numbers as a real OnlyKey. Host software routinely locates a raw-HID endpoint by
interface number — `python-onlykey` accepts `usage_page == 0xffab or
interface_number == 2`. With the console at MI_00/01 everything shifted up by
two and an app looking for MI_02 found the *keyboard* instead, then silently
talked to nothing. Windows binds the CDC by class and IAD, so its position does
not matter.

It adopts the attached key's identity — `1D50:60FC` for OnlyKey application
firmware. Windows recognises the FIDO usage page on its own, which is a good
sign the cloned report descriptors are being parsed correctly.

### Bootloader mode

The key's HalfKay bootloader is forwarded the same way, presented as
`16C0:0478` with a single vendor-defined HID interface. Verified identical to a
direct connection:

| | Direct | Through proxy |
|---|---|---|
| Usage page / usage | `0xFF9C` / `0x0021` | `0xFF9C` / `0x0021` |
| Input report | 0 bytes | 0 bytes |
| **Output report** | **1089 bytes** | **1089 bytes** |

### Power states

The device *mirrors the key*. Turning the host port off re-enumerates the
Feather without the HID interfaces:

| Host port | What the PC sees |
|---|---|
| on, key linked | console **+ the key's HID interfaces** |
| off | **console only** — the HID devices disappear |
| on again | detach, then console + HID back |

Each transition is a real detach/re-attach, so the PC treats it as an unplug.
The console survives because it is rebuilt into the reduced configuration, not
because it stays connected. Roughly two seconds each way.

---

## Controls

### Serial console

Open the COM port at 115200 (`pio device monitor`). Single keypresses, no Enter:

| Key | Action |
|---|---|
| `p` | power-cycle the host port (off 750 ms, on) |
| `0` / `1` | host port off (stays off) / on |
| `R` | **reboot the RP2040 — this is what puts the KEY into its bootloader** |
| `b` | pulse the bootloader contact; only wakes a key that is not enumerating |
| `i` | status: core health, link state, per-slot counters, button, pins |
| `h` | raw D+/D− pad levels and the pre-init probe result |
| `x` | re-print the report descriptors captured from the attached device |
| `d` | live descriptor dump straight off the device |
| `t` | toggle the 3-second status heartbeat |
| `?` | help |

The console disappears for ~2 s during any power transition, because the whole
USB device re-enumerates. It always comes back.

### BOOT button (GPIO 7)

**Toggles the host port power** — press once for off, again for on. Presses are
counted and shown in `i`, because the USB detach usually eats the log line
announcing the press.

On this board BOOT is a plain GPIO, not the flash-CS trick other RP2040 boards
use, so it reads normally at runtime. It only means "bootloader" when held down
*during reset*.

---

## Indicators

### Onboard LED (GPIO 13) — board state

| LED | Meaning |
|---|---|
| dark | working normally, or nothing plugged into the host port |
| flashing (400 ms) | host port powered down — proof the board is still alive |
| lit | something needs attention |

The heartbeat matters because cutting VBUS also detaches us from the PC, so
every other sign of life vanishes at the same moment.

"Needs attention" means: core 1 wedged or the host stack never started; a key is
attached but an interface failed to clone; or the PC has not mounted the proxy.
A fault must persist 2 s before it lights (`PROXY_LED_FAULT_MS`) — enumeration
and power cycles pass briefly through states that look faulty. A real fault
outranks the heartbeat.

### NeoPixel (GPIO 21) — traffic

Dark at rest. Each forwarded report pulses a colour, fading over 160 ms:

| Colour | Interface |
|---|---|
| white | keyboard |
| yellow | FIDO2 |
| green | OnlyKey app raw HID |
| red | seremu (Teensy debug) |
| **RGB cycle** | **anything unrecognised — i.e. the bootloader** |

seremu is **ranked below** the others. It chatters continuously (~27
reports/sec measured), so with plain last-writer-wins its red would repaint
every ~37 ms and nothing else would ever be visible. Unrecognised interfaces
rank *above* everything — if firmware is being flashed, that is the only thing
worth looking at. Set all entries in `kItfRank` equal for strict last-wins.

Brightness is capped at 40/255 (`PROXY_PIXEL_BRIGHTNESS`).

---

## Building and flashing

```bash
pio run -e adafruit_feather_rp2040_usb_host -t upload   # the proxy
pio run -e diag -t upload                               # diagnostics
pio run -e diag_trace -t upload                         # + TinyUSB trace
pio device monitor
```

| Environment | Purpose |
|---|---|
| `adafruit_feather_rp2040_usb_host` | the real proxy |
| `diag` | leaves the USB config alone, proxies nothing, reports what the host port sees. Self-driving: prints status every 12 s and forces re-enumeration **only when nothing is linked**. **Try this first when the proxy looks dead.** |
| `diag_trace` | `diag` plus TinyUSB's own enumeration trace. This is what found the 240 MHz bug. |

All builds expose a CDC interface, so the 1200-baud touch reset works and
uploads are automatic. If the firmware is wedged or built with
`PROXY_ENABLE_CDC=0`, hold **BOOT** and tap **RESET** to get the `RPI-RP2` drive.

**Every flash power-cycles the attached key** — GPIO 18 is undriven while the
RP2040 sits in its bootloader. So a key cannot be put into its bootloader
*before* flashing the Feather; it must be done after.

---

## Flashing the OnlyKey through the proxy

[`tools/halfkay_flash.py`](tools/halfkay_flash.py) drives a Teensy HalfKay
bootloader on Windows. It is a Python port of the parts of `teensy_loader_cli`
we need — that project must be compiled and there is no native compiler here,
and a scriptable flasher makes the proxy testable.

**Standalone**: stock Python plus `hid.dll`, no `teensy_loader_cli` checkout, no
build step, no third-party packages. Supply your own `.hex` by path.

```bash
python tools/halfkay_flash.py firmware.hex               # program, then reboot
python tools/halfkay_flash.py firmware.hex --no-reboot   # leave in bootloader
python tools/halfkay_flash.py --boot-only                # just reboot the key
```

It talks to `hid.dll` directly, so writes go out as control-pipe `SET_REPORT`s —
the same path the proxy forwards. Defaults target the OnlyKey's MK20DX256
(256 KB, 1024-byte blocks); `--code-size` / `--block-size` cover other parts.

Faithful to `teensy_loader_cli`: 3 address bytes + 61 pad + 1024-byte block,
always sends block 0 to trigger the chip erase and waits 3 s after it, skips
blank blocks, and finishes with the `FF FF FF` reboot packet.

**After any flash, check `PC->dev` in the `i` output.** It must read
`210 sent, 0 dropped` (or whatever the block count is) with **zero drops** — see
*back-pressure* below for why a drop there means a corrupted image rather than a
lost keystroke.

The official Teensy Loader GUI works through the proxy too; the CLI tool just
makes it repeatable.

---

## ⚠ The 240 MHz requirement

**`board_build.f_cpu = 240000000L` is required, not a preference.**

Pico-PIO-USB documents 120 MHz as supported and it satisfies the "multiple of
12 MHz" rule, so it looks correct. It is not. At 120 MHz the OnlyKey enumerates
this far and then stops dead:

```
[1:] USBH Device Attach
Full Speed
Get 8 byte of Device Descriptor
[:0] on EP 00 with 8 bytes: OK      <- setup OK
[:0] on EP 80 with 8 bytes: OK      <- data OK
[:0] on EP 00 with 0 bytes: OK      <- status OK
Set Address = 1
[1:0] Set Address: 00 05 01 00 ...
[:0] on EP 00 with 8 bytes: OK      <- setup OK ... then nothing, forever
```

The SET_ADDRESS control transfer never completes — no status stage, no timeout,
no retry. The state machine simply stalls, so no class driver binds and the
device is silently useless. At 240 MHz it enumerates every time.

This is an overclock past the 133 MHz default; the core bumps vreg to 1.15 V
automatically above that. Stable throughout testing, but it is above spec.

---

## The vendored TinyUSB copy

`lib/Adafruit_TinyUSB_Arduino/` is a **patched copy** of the library bundled
with the core, and the project will not work without it. Every change is marked
`// PATCHED`.

All of them exist because HalfKay writes firmware in **1089-byte** control
`SET_REPORT`s, which the stock device stack rejects:

```c
case HID_REQ_CONTROL_SET_REPORT:
  TU_VERIFY(request->wLength <= CFG_TUD_HID_EP_BUFSIZE);   // 64 -> stall
```

`CFG_TUD_HID_EP_BUFSIZE` is defined **unguarded** in two places, so `-D` cannot
override it, and `tusb_config.h` cannot be shadowed either — the library
quote-includes its own copy from its own directory, which always wins.
Vendoring is the only route.

Raising it to 1152 works, but **that one constant is overloaded four ways**, and
each one produced a distinct, hard-to-diagnose failure:

| Where it is used | Symptom when set to 1152 | Fix |
|---|---|---|
| Endpoint `wMaxPacketSize` in the interface descriptor | Windows: *Invalid Configuration Descriptor*, whole device rejected | pinned to 64 in `Adafruit_USBD_HID::makeItfDesc` |
| Control `SET_REPORT` size limit | firmware blocks stalled | **needs 1152** — the original goal |
| Interrupt OUT endpoint arming | OUT reports never delivered at all | new `CFG_TUD_HID_EP_OUT_PACKET` (64) in `hid_device.c` |

The third is the subtlest. `hidd_open()` and the OUT completion handler arm the
endpoint with a `CFG_TUD_HID_EP_BUFSIZE`-sized request. A 64-byte packet on a
64-byte endpoint is **not a short packet**, so a 1152-byte request never
completes — the stack waits for 18 packets before delivering anything. Control
writes kept working while interrupt writes silently vanished, which made it look
like a host-software problem.

To re-vendor after a core update, copy `src/` and `library.properties` from
`framework-arduinopico/libraries/Adafruit_TinyUSB_Arduino/` and re-apply all
three.

---

## How it works

### Two cores

Core 1 runs the Pico-PIO-USB **host** stack (bit-banged on GPIO 16/17). Core 0
runs the TinyUSB **device** stack. The hard rule: **only core 1 may call `tuh_*`,
only core 0 may call `tud_*` or `TinyUSBDevice` methods.**

Reports cross between them through **per-interface** lock-free SPSC ring
buffers. Per-interface, not shared, so a host that stops draining one interface
cannot block the others — CTAPHID in particular has timeouts that head-of-line
blocking would trip.

Core 1 records which step of its loop it entered last (`last phase:` in `i`), so
a stall reports *where* it died instead of just a frozen counter.

### Descriptor cloning

Report descriptors are captured from the attached device at mount time and
installed verbatim, so anything that enumerates is proxied. The compiled-in
descriptor table is used **only to name and colour** the familiar interfaces;
unrecognised ones are cloned just as readily and dumped in `\xNN` form.

VID/PID is adopted from the device, which is what lets a flashing tool see
`16C0:0478` when the key is in its bootloader.

### Presence mirroring

A USB device **cannot** detach some interfaces and keep others — `tud_disconnect()`
drops the whole device. So the configuration descriptor is rebuilt while
detached:

1. `tud_disconnect()` — everything goes away
2. `clearConfiguration()`, re-add CDC, then the HID interfaces only if a key is
   linked, using the captured descriptors
3. `tud_connect()` — the PC enumerates whatever is now described

Two non-obvious constraints live in that path:

- `Adafruit_USBD_HID::begin()` and `Adafruit_USBD_CDC::begin()` **early-return
  once their instance is valid**, so they cannot re-add themselves to a cleared
  configuration. `begin()` is called once at boot to claim instance indices;
  every rebuild afterwards uses `addInterface()`.
- `clearConfiguration()` wipes the device-level class bytes (`bDeviceClass`) and
  there is no public setter. Windows still binds `usbser.sys` to a CDC-only
  configuration with `bDeviceClass = 0` — verified empirically, not assumed.

### Back-pressure, and where it must NOT be applied

**This is the most important pair of details in the project.**
`set_report_thunk()` runs from the device stack, and by the time it is called
the host has *already been ACKed*. Returning without queueing therefore tells
the host "delivered" while discarding the data — silent loss the sender believes
succeeded. With an earlier 4-slot queue a firmware flash lost **162 of 210
blocks**; both the official Teensy Loader and the CLI tool reported success and
wrote a corrupt image, which looked for hours like a bootloader problem.

The obvious fix — block until a slot frees — is correct for firmware blocks and
**catastrophic for everything else**, because that callback runs in **USB IRQ
context** and `tud_task()` is what re-arms the interrupt OUT endpoint after each
packet. Blocking there prevents the very draining it waits for: the first few
packets land, the queue backs up, and interrupt OUT wedges permanently. Raw HID
traffic died after ~3 reports while control transfers kept working.

So the rule is split by size:

- **> `PROXY_SMALL_REPORT_MAX` (64 bytes)** — a control-pipe firmware block, from
  a synchronous host sending one at a time with core 1 otherwise idle. Waits up
  to `PROXY_OUT_BLOCK_MS` for a slot. Losing one corrupts firmware.
- **<= 64 bytes** — an ordinary HID report. **Never blocks.** The queue is deep
  enough that core 1 keeps up, and a dropped keystroke is survivable.

Verified: 20 consecutive raw-HID writes all forwarded with a reply for each, and
a full firmware flash at `PC->dev : 210 sent, 0 dropped`.

If you touch the OUT path, check `PC->dev` after both a flash *and* a burst of
raw-HID traffic. The two exercise opposite halves of this rule.

### IN endpoint arming

Interfaces are polled only after `PROXY_ARM_DELAY_MS` (500 ms), so a device that
has just enumerated is not hit with an IN request while still initialising.

`arm_service()` keeps a request outstanding at all times rather than arming
once. An earlier version armed a single time per mount and relied on the report
callback to re-arm; a single failed re-arm silenced that interface permanently,
which stalled forwarding after a few hundred reports. `tuh_hid_receive_ready()`
is false while a request is pending, so this only re-issues when the chain has
genuinely broken. The `rearm` counter in `i` shows when that happens.

### Entering the key's bootloader — the exact procedure

The key samples its bootloader contact **only at power-up**. Grounding it while
the key is running does nothing; this was measured, including a 3-second hold.
Two consequences follow, and both are counter-intuitive:

- **`b` will not put a running key into the bootloader.** It is only useful for
  a key that is not enumerating at all.
- **The `p` / `0` / `1` VBUS commands cannot help either.** Phantom power through
  D+/D− means cutting VBUS does not actually de-power the key, so it never gets
  the fresh startup at which the contact is read.

What works is **`R`** — reboot the RP2040 — because an RP2040 reset happens to do
both required things at once:

1. GPIO 18 goes undriven, so the load switch genuinely opens and the key really
   loses power (unlike the VBUS commands).
2. GPIO 6 reverts to input-with-pull-down, holding the contact grounded across
   the key's power-up.

So the exact sequence is:

```bash
# 1. put the key into its bootloader
#    press R in the console, or simply reflash the Feather -- same mechanism
#    verify: the port reappears as 16C0:0478

# 2. flash
python tools/halfkay_flash.py firmware.hex

# 3. verify
#    the port returns as 1D50:60FC with all interfaces cloned, and
#    PC->dev in the `i` output must read "N sent, 0 dropped"
```

**`R` is direct-wire only.** With a transistor the same reset-time pull-down
holds the gate *off*, releasing the contact, so the key boots normally instead —
which is the safer behaviour for everyday use but means bootloader entry then
needs the key's own button.

### Recovering a key that is not enumerating

A key powered down *while in its bootloader* comes back with **no USB
descriptors at all** — silent on the bus, indistinguishable from an empty port.
It is not broken; the contact needs poking.

When the port stays silent with power on, the firmware pulses the contact, up to
3 attempts, then stops rather than poking forever. The wait depends on history:

- **Nothing ever enumerated** → `PROXY_BOOTSEL_AUTO_MS` (6 s). Genuinely stuck.
- **A device appeared and went away** → `PROXY_BOOTSEL_REBOOT_GRACE_MS` (30 s).
  It is probably rebooting into firmware just flashed through us, and reboots
  can take well over 6 s. Poking then knocks it back into the bootloader, which
  loops forever — an earlier 6 s-for-everything rule did exactly that after
  every flash.

The distinction is a *longer wait*, not a refusal: an earlier version latched
"seen a device" and never cleared it on a physical unplug, leaving a genuinely
stuck key unrecoverable.

### Logging

All log output goes into a ring buffer (`include/log_ring.h`) drained from
`loop()` on core 0. This is required, not tidiness:

- Core 1 logs mounts and power events; writing to the CDC from core 1 would race
  core 0's `tud_task()` on the device FIFOs.
- `Adafruit_USBD_CDC::write` spins on `yield()` when a terminal is connected but
  not reading.
- With `-DCFG_TUSB_DEBUG`, Adafruit's `log_printf` runs in **USB IRQ context**.
  Writing to a Serial object there deadlocks the whole device stack once nothing
  drains the port: the FIFO fills, the write blocks in the IRQ, control
  transfers stop being answered, and the host can no longer even *open* the
  port. Recovery needs a physical replug. (Learned the hard way.)

### NeoPixel driver

Driven by a **PIO1** state machine, deliberately pinned — Pico-PIO-USB owns PIO0
state machines 0–2, so PIO1 can never collide with the USB host.
`pio_can_add_program()` is checked first, so a busy PIO1 disables the light
rather than hanging the board.

This also sidesteps the usual warning that NeoPixels need uninterrupted timing:
that applies to bit-banged drivers which mask interrupts and count CPU cycles.
Here the waveform is generated in hardware. All PIO access happens on core 0.

---

## Configuration

Top of `src/main.cpp`.

| Define | Default | Meaning |
|---|---|---|
| `PROXY_ENABLE_CDC` | `1` | Include the CDC console. `0` gives a HID-only clone with the console on the UART (GPIO 0/1) — independent of USB, needs a USB-TTL adapter |
| `PROXY_BOOTSEL_PIN` | `6` | GPIO wired to the OnlyKey bootloader contact |
| `PROXY_BOOTSEL_ACTIVE_HIGH` | `0` | `0` = direct wire (open-drain), `1` = via transistor. Wrong value bricks bootup — see Hardware |
| `PROXY_BOOTSEL_AUTO` | `1` | Auto-pulse the contact when the port stays silent |
| `PROXY_MAX_OUT_REPORT` | `1152` | Largest host→device report; sized for HalfKay's 1089 bytes |
| `PROXY_OUT_QUEUE_LEN` | `8` | Host→device queue depth. Too shallow silently corrupts firmware writes |
| `PROXY_OUT_BLOCK_MS` | `250` | How long a large report waits for a free slot (see *back-pressure*) |
| `PROXY_SMALL_REPORT_MAX` | `64` | At or below this, a report never blocks the IRQ |
| `PROXY_IN_QUEUE_LEN` | `32` | Device→PC queue depth per interface |
| `PROXY_BOOTSEL_REBOOT_GRACE_MS` | `30000` | Silence tolerated before poking a key that has already enumerated once |
| `PROXY_ARM_DELAY_MS` | `500` | Settle time before first polling an interface |
| `PROXY_POWER_OFF_MS` | `750` | Off duration for `p` |
| `PROXY_MAX_HID` | `4` | Must be ≤ `CFG_TUD_HID` in `platformio.ini` |
| `PROXY_PIXEL_BRIGHTNESS` | `40` | NeoPixel ceiling, 0–255 |
| `PROXY_LED_FAULT_MS` | `2000` | How long a fault must persist before GPIO 13 lights |
| `PROXY_CUT_DATA_LINES` | `0` | See *phantom power* below — **known to wedge the host stack** |

---

## Known issues

**Phantom power.** Cutting VBUS does *not* de-power the attached key. PIO keeps
driving D+/D−, and current flows through the device's ESD clamp diodes into its
supply rail, so the key stays lit and never truly resets. Symptoms that look
like a power cycle (host stack reporting `0 HID itf bound`, `GPIO18 reads 0`)
only mean the device left the *bus* — not that it lost power.

`PROXY_CUT_DATA_LINES=1` releases the pads to remove that path, but it **wedges
Pico-PIO-USB**: the host stack stops servicing and the command that would
restore power never runs, so the board needs a manual reset. Doing it safely
needs the PIO state machines stopped first. Off by default.

Consequence: a true power-cycle of the key requires physically unplugging it.
The `p` / `0` / `1` commands drop it off the bus, which is enough to re-enumerate
it, but not enough to make it re-run its startup — so they cannot be used to
enter the bootloader by "power up with the button held". Use the `b` command,
which reboots a running key into the bootloader directly.

**A key that lost power while in its bootloader comes back with no USB
descriptors at all** — silent on the bus, indistinguishable from an empty port.
It is not broken; it needs the bootloader contact poked, which is what the
auto-recovery above does. Without the GPIO 6 wiring, unplug it and re-insert it
while holding its bootloader button.

**The bootloader contact is sampled only at power-up.** Grounding it on a
running key does nothing, so `b` cannot trigger bootloader entry and neither can
the VBUS commands. Use `R`. See *Entering the key's bootloader* above.

**Core 1 stalls.** Core 1 has wedged during device mode transitions, in both the
proxy and `diag` builds. Recovery requires a reset — which power-cycles the key.
The `last phase:` field in `i` reports where it stopped; not yet root-caused.

**GET_REPORT stalls.** The host-side get is asynchronous and the device-side
callback must answer synchronously, so control-pipe `GET_REPORT` returns a
stall. Affects Feature reports on the keyboard and seremu interfaces; CTAPHID
and the OnlyKey app use interrupt transfers.

**Interface numbering.** HID is emitted first so the cloned interfaces match a
real OnlyKey (rawhid at MI_02); the console takes the tail. Do not reorder
`usb_build_config()` — host software does locate raw HID by interface number,
and getting this wrong makes apps talk to the keyboard instead.

**Descriptor size ceiling.** Report descriptors larger than
`CFG_TUH_ENUMERATION_BUFSIZE` (256) are not delivered by TinyUSB and such an
interface cannot be cloned.

---

## Status

**Verified working:**

- Application mode: all interfaces cloned and linked, PC mounts the proxy
- Bootloader mode: HalfKay cloned as `16C0:0478`, HID capabilities byte-identical
  to a direct connection
- **A complete, correct firmware flash through the proxy** — 210 blocks, 215 KB,
  `PC->dev : 210 sent, 0 dropped`, after which the key boots the new firmware
- **Bidirectional raw HID with real OnlyKey software** — `python-onlykey`
  connects, sends commands and reads protocol replies through the proxy
  (`Error OnlyKey must be initialized first` is the key answering, not a
  transport failure)
- 20 consecutive interrupt-OUT writes forwarded with a reply for each, zero drops
- Automatic recovery of a descriptor-less key via the bootloader contact
- **The full cycle exercised end to end**: bootloader entry, flash, reboot,
  return to application mode, raw-HID response — repeatedly, zero drops
- Power control by console and BOOT button; console-only re-enumeration
- Clean build under `-Wall -Wextra` across all three environments

**Not verified — needs a human at the keyboard:**

- **Keystrokes from the OnlyKey actually reaching the PC** (expect a white pulse)
- **A WebAuthn / FIDO2 login through the proxy** (expect yellow)
- **The OnlyKey desktop app talking to the key** (expect green)

Report counters prove bytes move; they cannot prove the host *interprets* them.

**Notes on reading the counters:** `dev->PC` sitting still is not necessarily a
fault — the keyboard, FIDO2 and raw HID interfaces only send on user action, and
seremu goes quiet once it has flushed its startup output. The `rearm` column is
the real health signal: non-zero means an IN request chain broke and had to be
restarted.
