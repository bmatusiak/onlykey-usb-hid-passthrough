# OnlyKey USB HID passthrough

A USB HID proxy for the **Adafruit Feather RP2040 with USB Type-A Host**. An
OnlyKey plugs into the Feather's Type-A host port; the Feather presents itself to
the PC as that same OnlyKey and forwards every HID report in both directions.

The point of the exercise is the **power pin**: GPIO 18 gates 5 V to the Type-A
port, so the key can be powered down and back up under software control, and the
PC sees a genuine unplug/replug rather than a device that quietly stops
responding.

```
   PC  <=====[ native USB ]=====  Feather RP2040  =====[ Type-A host ]=====>  OnlyKey
                device stack                                 host stack
                  (core 0)                                    (core 1)
```

---

## Hardware

**Adafruit Feather RP2040 with USB Type A Host** — <https://www.adafruit.com/product/5723>

Nothing else is required: the Type-A host connector, the switchable 5 V supply
to it, the NeoPixel and the BOOT button are all on the board. Two USB cables in
total — one to the PC, and the OnlyKey into the Type-A port.

Pins used, all from the board's variant definition rather than hardcoded:

| GPIO | Role |
|---|---|
| 16 / 17 | USB host D+ / D− (bit-banged by PIO) |
| 18 | 5 V enable for the Type-A port — the power control |
| 13 | onboard LED (board state) |
| 21 | NeoPixel (traffic) |
| 20 | NeoPixel power |
| 7 | BOOT button (plain GPIO at runtime) |
| 0 / 1 | UART TX / RX — only used as the console when `PROXY_ENABLE_CDC=0` |

The one accessory worth having is a **USB-TTL serial adapter** for GPIO 0/1, and
only if you build with `PROXY_ENABLE_CDC=0`. In the default build the console
comes over the same USB cable and no adapter is needed.

The board must be the USB-Host variant. A plain Feather RP2040 has no Type-A
connector and no 5 V load switch, so none of this applies to it.

---

## What the PC sees

With the key attached and powered:

| Interface | Windows name | Purpose |
|---|---|---|
| MI_00/01 | USB Serial Device (COM*n*) | control console (see below) |
| MI_02 | HID Keyboard Device | keyboard |
| MI_03 | HID-compliant fido | FIDO2 / U2F (CTAPHID) |
| MI_04 | HID-compliant vendor-defined | OnlyKey app raw HID |
| MI_05 | HID-compliant vendor-defined | Teensy seremu debug |

It enumerates as `16C0:0486`, `CRYPTOTRUST` / `ONLYKEY`, serial `1000000000` —
matching the known-good `libcomposite` gadget configuration. The real OnlyKey is
`1D50:60FC`; change `PROXY_VID` / `PROXY_PID` in `src/main.cpp` if something on
your system filters on those.

Windows recognises the FIDO usage page on its own ("HID-compliant fido"), which
is a good sign the cloned report descriptors are being parsed correctly.

### Power states

The device *mirrors the key*. Turning the host port off doesn't just stop
forwarding — it re-enumerates the Feather without the HID interfaces:

| Host port | What the PC sees |
|---|---|
| on, key linked | console **+ all four HID interfaces** |
| off | **console only** — the HID devices disappear |
| on again | detach, then console + all four HID back |

Each transition is a real detach/re-attach, so the PC treats it as an unplug.
The console survives because it is rebuilt into the reduced configuration, not
because it stays connected — see [How presence mirroring works](#how-presence-mirroring-works).

Re-enumeration takes roughly two seconds in each direction.

---

## Controls

### Serial console

Open the COM port at 115200 (`pio device monitor`). Single keypresses, no Enter:

| Key | Action |
|---|---|
| `p` | power-cycle the host port (off 750 ms, on) |
| `0` | host port off — stays off |
| `1` | host port on |
| `i` | status: core health, link state, counters, button, pins |
| `h` | raw D+/D− pad levels and the pre-init probe result |
| `d` | dump the attached device's descriptors |
| `t` | toggle the 3-second status heartbeat |
| `?` | help |

The console disappears for ~2 s during any power transition, because the whole
USB device re-enumerates. It always comes back.

### BOOT button (GPIO 7)

**Toggles the host port power** — press once for off, again for on. Presses are
counted and shown in `i`, which matters because the USB detach usually eats the
log line announcing the press.

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
attached but fewer than four interfaces matched; or all four matched but the PC
hasn't mounted the proxy. A fault must persist for 2 s before it lights
(`PROXY_LED_FAULT_MS`) — enumeration and power cycles pass briefly through
states that look faulty, and a light that flickers in normal use is one you
learn to ignore. A real fault outranks the heartbeat.

### NeoPixel (GPIO 21) — traffic

Dark at rest. Each forwarded report pulses a colour identifying the interface,
fading back over 160 ms:

| Colour | Interface |
|---|---|
| white | keyboard |
| yellow | FIDO2 |
| green | OnlyKey app raw HID |
| red | seremu (Teensy debug) |

seremu is **ranked below** the others. It chatters continuously (~27 reports/sec
measured), so with plain last-writer-wins its red would repaint every ~37 ms and
nothing else would ever be visible. Ranked this way it still flashes red when the
link is otherwise idle, but real traffic always shows through. Set all entries in
`kItfRank` equal for strict last-wins.

Brightness is capped at 40/255 (`PROXY_PIXEL_BRIGHTNESS`) — these are bright.

---

## Building and flashing

Three environments:

```bash
pio run -e adafruit_feather_rp2040_usb_host -t upload   # the proxy
pio run -e diag -t upload                               # diagnostics
pio run -e diag_trace -t upload                         # + TinyUSB trace
pio device monitor
```

| Environment | Purpose |
|---|---|
| `adafruit_feather_rp2040_usb_host` | the real proxy |
| `diag` | leaves the USB config alone, proxies nothing, reports what the host port sees. Self-driving: prints status and forces re-enumeration every 12 s, so a monitor attached at any time gets a full cycle with no interaction. **Try this first when the proxy looks dead.** |
| `diag_trace` | `diag` plus TinyUSB's own enumeration trace. This is what found the 240 MHz bug. |

All builds expose a CDC interface, so the 1200-baud touch reset works and
uploads are automatic. If the firmware is wedged or built with
`PROXY_ENABLE_CDC=0`, fall back to holding **BOOT** and tapping **RESET** to get
the `RPI-RP2` drive, then copy `firmware.uf2` onto it.

---

## ⚠ The 240 MHz requirement

**`board_build.f_cpu = 240000000L` is required, not a preference.**

Pico-PIO-USB documents 120 MHz as supported and it satisfies the "multiple of
12 MHz" rule, so it looks correct. It is not. At 120 MHz the OnlyKey enumerates
this far and then stops dead:

```
[1:] USBH Device Attach
Full Speed
[1:0] Open EP0 with Size = 8
Get 8 byte of Device Descriptor
[:0] on EP 00 with 8 bytes: OK      <- setup OK
[:0] on EP 80 with 8 bytes: OK      <- data OK
[:0] on EP 00 with 0 bytes: OK      <- status OK
Set Address = 1
[1:0] Set Address: 00 05 01 00 ...
[:0] on EP 00 with 8 bytes: OK      <- setup OK ... then nothing, forever
```

The SET_ADDRESS control transfer never completes — no status stage, no timeout,
no retry. The host state machine simply stalls, so no class driver ever binds
and the device is silently useless. At 240 MHz the same key enumerates every
time and all four interfaces bind.

This costs an overclock past the 133 MHz default; the core bumps vreg to 1.15 V
automatically above that. It has been stable throughout testing, but it is
running above spec — revert to `120000000L` if you'd rather not, accepting that
the OnlyKey will not enumerate.

---

## How it works

### Two cores

Core 1 runs the Pico-PIO-USB **host** stack (bit-banged on GPIO 16/17). Core 0
runs the TinyUSB **device** stack. The hard rule: **only core 1 may call `tuh_*`,
only core 0 may call `tud_*` or `TinyUSBDevice` methods.**

Reports cross between them through **per-interface** lock-free SPSC ring buffers.
Per-interface, not shared, so a host that stops draining one interface cannot
block the others — CTAPHID in particular has timeouts that head-of-line blocking
would trip.

### Interface matching

The four report descriptors are compiled in, taken from `usbhid-dump -m 16c0` of
a real OnlyKey. On mount, each host-side HID interface is identified by matching
its descriptor — exact first, then by the leading usage-page bytes. Anything
unrecognised is dumped in `\xNN` form so it can be pasted into the table in
`src/main.cpp`.

### How presence mirroring works

A USB device **cannot** detach some interfaces and keep others — interfaces are
fixed by the configuration, and `tud_disconnect()` drops the whole device. So the
console can't simply "stay up" across a power transition.

Instead the configuration descriptor is rebuilt while detached:

1. `tud_disconnect()` — everything goes away
2. `clearConfiguration()`, then add back CDC, and the four HID interfaces only if
   a key is linked
3. `tud_connect()` — the PC enumerates whatever is now described

From the PC's point of view that's a real unplug followed by a different device
appearing. That's what makes "console only while the port is off" possible.

Two non-obvious constraints live in that path:

- `Adafruit_USBD_HID::begin()` and `Adafruit_USBD_CDC::begin()` **early-return
  once their instance is valid**, so they cannot re-add themselves to a cleared
  configuration. `begin()` is called exactly once at boot to claim instance
  indices; every rebuild afterwards uses `addInterface()`. "Simplifying" this
  silently produces a configuration with missing interfaces.
- `clearConfiguration()` also wipes the device-level class bytes
  (`bDeviceClass`), and there is no public setter to restore them. Windows still
  binds `usbser.sys` to a CDC-only configuration with `bDeviceClass = 0` —
  verified empirically, not assumed.

### Logging

All log output goes into a ring buffer (`include/log_ring.h`) and is drained to
the port from `loop()` on core 0. This is not tidiness — it is required:

- Core 1 logs mounts and power events; writing to the CDC from core 1 would race
  core 0's `tud_task()` on the device FIFOs.
- `Adafruit_USBD_CDC::write` spins on `yield()` when a terminal is connected but
  not reading.
- With `-DCFG_TUSB_DEBUG`, Adafruit's `log_printf` runs in **USB IRQ context**.
  Writing to a Serial object there deadlocks the whole device stack the moment
  nothing drains the port: the FIFO fills, the write blocks in the IRQ, control
  transfers stop being answered, and the host can no longer even *open* the port.
  Recovering needs a physical replug. (Learned the hard way; see
  `[env:diag_trace]` in `platformio.ini`.)

### NeoPixel driver

Driven by a **PIO1** state machine, deliberately pinned. Pico-PIO-USB owns PIO0
state machines 0–2, so PIO1 can never collide with the USB host. `pio_can_add_program()`
is checked first, so a busy PIO1 silently disables the light rather than hanging
the board.

This also sidesteps the usual warning that NeoPixels need uninterrupted timing:
that applies to bit-banged drivers, which mask interrupts and count CPU cycles.
Here the waveform is generated in hardware. All PIO access still happens only on
core 0.

---

## Configuration

Everything below lives at the top of `src/main.cpp`.

| Define | Default | Meaning |
|---|---|---|
| `PROXY_ENABLE_CDC` | `1` | Include the CDC console. `0` gives a byte-exact HID-only clone with the console on the UART (GPIO 0 TX / GPIO 1 RX) — fully independent of USB, needs a USB-TTL adapter. |
| `PROXY_VID` / `PROXY_PID` | `0x16C0` / `0x0486` | Identity shown to the PC |
| `PROXY_POWER_OFF_MS` | `750` | Off duration for `p` |
| `PROXY_SETTLE_MS` | `150` | Quiet period after the last interface mounts before attaching, so the PC enumerates once rather than four times |
| `PROXY_MAX_HID` | `4` | Must be ≤ `CFG_TUD_HID` in `platformio.ini` |
| `PROXY_IN_QUEUE_LEN` / `PROXY_OUT_QUEUE_LEN` | `16` / `8` | Ring depth per interface |
| `PROXY_PIXEL_BRIGHTNESS` | `40` | NeoPixel ceiling, 0–255 |
| `PROXY_LED_FAULT_MS` | `2000` | How long a fault must persist before GPIO 13 lights |

---

## Troubleshooting

**Nothing happens, no COM port.** Flash `diag` and read the log. It reports core
1 liveness, whether the host stack started, VBUS state, and the pre-init D+/D−
probe. If `core1` shows `STALLED` or `host stack: NOT STARTED`, the problem is on
the host side before anything HID-related.

**The key never enumerates.** Flash `diag_trace` and watch a cycle. If it stops
after `Set Address = 1`, that's the 240 MHz problem above.

**Some interfaces don't match.** The log prints `[map] unrecognised descriptor`
followed by the raw bytes. Paste them into the descriptor table in
`src/main.cpp`.

**Reading the port hangs, or "access denied" / "device does not recognize the
command".** Use pyserial rather than PowerShell's `System.IO.Ports`, and **never
write the modem control lines** (`ser.dtr` / `ser.rts`) — on this `usbser.sys`
stack that wedges the port until the device is physically replugged. Read-only
access is reliable.

**Build fails oddly in `msc_device.c` or `video_device.c`.** A second copy of
Adafruit TinyUSB has been installed into `.pio/libdeps` and is shadowing the
one bundled with the core. Pico-PIO-USB pulls it in via `depends=`. Delete the
`.pio/libdeps/*/Adafruit TinyUSB Library` directories.

**`CFG_TUD_HID redefined` warning.** Harmless. The core's `include/tusb_config.h`
hardcodes it to 2 without a guard, but those translation units compile to weak
stubs under `USE_TINYUSB`; the real implementation comes from the bundled
Adafruit library, whose config does guard the value.

---

## Status

Verified working:

- All four interfaces match and link; PC mounts the proxy
- Reports flow both directions (counters climb, zero drops)
- Power off/on and power-cycle, by console and by button
- Console-only re-enumeration while the host port is off, and recovery from it
- Board survives repeated power cycles without intervention

Not verified — these need a human at the keyboard:

- **Keystrokes from the OnlyKey actually reaching the PC** (expect a white pulse)
- **A WebAuthn / FIDO2 login completing through the proxy** (expect yellow)
- **The OnlyKey desktop app talking to the key** (expect green)

Known limitations:

- `GET_REPORT` over the control pipe returns a stall. The host-side get is
  asynchronous and the device-side callback must answer synchronously. This
  affects the Feature reports on the keyboard and seremu interfaces; CTAPHID and
  the OnlyKey app both use interrupt transfers, so normal operation shouldn't
  touch it.
- With `PROXY_ENABLE_CDC=1` the HID interfaces sit at MI_02–05 rather than
  MI_00–03, so it isn't a byte-exact clone. Nothing binds by interface number —
  Windows matches FIDO2 by usage page, the OnlyKey app by VID/PID — but set
  `PROXY_ENABLE_CDC=0` if you need exactness.
- Report descriptors larger than `CFG_TUH_ENUMERATION_BUFSIZE` (256) are not
  delivered by TinyUSB and such an interface cannot be matched. The OnlyKey's are
  well under.
