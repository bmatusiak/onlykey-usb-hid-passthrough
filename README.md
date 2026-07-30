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

Set `PROXY_BOOTSEL_ACTIVE_HIGH` to `1` for a transistor (drive high to press,
the default) or `0` for a direct connection (open-drain, never driven high).

---

## What the PC sees

### Application mode

| Interface | Windows name | Purpose |
|---|---|---|
| MI_00/01 | USB Serial Device (COM*n*) | control console |
| MI_02 | HID Keyboard Device | keyboard |
| MI_03 | HID-compliant fido | FIDO2 / U2F (CTAPHID) |
| MI_04 | HID-compliant vendor-defined | OnlyKey app raw HID |
| MI_05 | HID-compliant vendor-defined | Teensy seremu debug |

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
| `b` | tap the bootloader contact — reboots a running key into its bootloader |
| `B` | hold the contact through a power cycle — for keys that only sample it at startup |
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
with the core, and the project will not work without it. Two one-line changes,
both marked `// PATCHED`:

**1. `CFG_TUD_HID_EP_BUFSIZE` 64 → 1152.** HalfKay writes firmware in 1089-byte
`SET_REPORT`s. The device stack rejected them outright:

```c
case HID_REQ_CONTROL_SET_REPORT:
  TU_VERIFY(request->wLength <= CFG_TUD_HID_EP_BUFSIZE);   // 64 -> stall
```

That constant is defined **unguarded** in two places, so `-D` cannot override
it, and `tusb_config.h` cannot be shadowed either — the library quote-includes
its own copy from its own directory, which always wins. Vendoring is the only
route.

**2. Endpoint packet size pinned to 64** in `Adafruit_USBD_HID::makeItfDesc`.
That same constant was also passed as the endpoint's `wMaxPacketSize`. At 1152
that is illegal for a full-speed interrupt endpoint (64 max) and Windows
rejected the entire configuration with *Invalid Configuration Descriptor*. The
buffer and the packet size are unrelated and now decoupled.

To re-vendor after a core update, copy `src/` and `library.properties` from
`framework-arduinopico/libraries/Adafruit_TinyUSB_Arduino/` and re-apply both.

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

### IN endpoint arming

Interfaces are polled only after `PROXY_ARM_DELAY_MS` (500 ms), so a device that
has just enumerated is not hit with an IN request while still initialising.

`arm_service()` keeps a request outstanding at all times rather than arming
once. An earlier version armed a single time per mount and relied on the report
callback to re-arm; a single failed re-arm silenced that interface permanently,
which stalled forwarding after a few hundred reports. `tuh_hid_receive_ready()`
is false while a request is pending, so this only re-issues when the chain has
genuinely broken. The `rearm` counter in `i` shows when that happens.

### Bootloader auto-recovery

A key powered down *while in its bootloader* comes back with **no USB
descriptors at all** — silent on the bus, indistinguishable from an empty port.
It is not broken; it just needs the bootloader contact poked.

When the port stays silent for `PROXY_BOOTSEL_AUTO_MS` (6 s) with power on, the
firmware pulses the contact, up to 3 attempts, then stops rather than poking
forever. This requires the GPIO 6 wiring above; without it, recover by hand.

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
| `PROXY_BOOTSEL_ACTIVE_HIGH` | `1` | `1` = via transistor (drive high), `0` = direct open-drain |
| `PROXY_BOOTSEL_AUTO` | `1` | Auto-pulse the contact when the port stays silent |
| `PROXY_MAX_OUT_REPORT` | `1152` | Largest host→device report; sized for HalfKay's 1089 bytes |
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

**Core 1 stalls.** Core 1 has wedged during device mode transitions, in both the
proxy and `diag` builds. Recovery requires a reset — which power-cycles the key.
The `last phase:` field in `i` reports where it stopped; not yet root-caused.

**GET_REPORT stalls.** The host-side get is asynchronous and the device-side
callback must answer synchronously, so control-pipe `GET_REPORT` returns a
stall. Affects Feature reports on the keyboard and seremu interfaces; CTAPHID
and the OnlyKey app use interrupt transfers.

**Interface numbering.** With `PROXY_ENABLE_CDC=1` the HID interfaces sit at
MI_02+ rather than MI_00+. Nothing binds by interface number — Windows matches
FIDO2 by usage page, the OnlyKey app by VID/PID — but set `PROXY_ENABLE_CDC=0`
if you need an exact clone.

**Descriptor size ceiling.** Report descriptors larger than
`CFG_TUH_ENUMERATION_BUFSIZE` (256) are not delivered by TinyUSB and such an
interface cannot be cloned.

---

## Status

**Verified working:**

- Application mode: all four interfaces cloned and linked, PC mounts the proxy,
  reports forwarding both directions
- Bootloader mode: HalfKay cloned and presented as `16C0:0478`, HID capabilities
  byte-identical to a direct connection
- Automatic recovery of a descriptor-less key via the bootloader contact
- Power off/on and power-cycle, by console and by BOOT button
- Console-only re-enumeration while the host port is off, and recovery from it

**Not verified — needs a human at the keyboard:**

- **Keystrokes from the OnlyKey actually reaching the PC** (expect a white pulse)
- **A WebAuthn / FIDO2 login through the proxy** (expect yellow)
- **The OnlyKey desktop app talking to the key** (expect green)
- **An actual firmware flash through the proxy.** Buffers are sized correctly
  and enumeration is proven, but a flash is hundreds of sequential 1089-byte
  transfers through the cross-core queues, each re-issued as a fresh control
  transfer, and the flashing tool has timeouts. Sustained throughput has not
  been measured.
- **Sustained forwarding after the arming fix.** The self-healing re-arm is
  implemented but was never exercised in application mode.
