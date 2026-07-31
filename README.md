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
| `b` | press/release the contact — **only works on a key that is RUNNING firmware** |
| `B` | **cold bootloader entry: power-cycle, let it boot, then press/release** — recovers a key with no firmware |
| `G` / `g` | ground the contact and hold / release it (characterising only — holding *prevents* the trigger) |
| `A` | toggle automatic recovery; off stops the board power-cycling itself |
| `R` | reboot the RP2040; also recovers a stuck key |
| `i` | status: core health, link state, per-slot counters, button, pins |
| `h` | raw D+/D− pad levels and the pre-init probe result |
| `x` | re-print the report descriptors captured from the attached device |
| `d` | live descriptor dump straight off the device |
| `t` | toggle the 3-second status heartbeat |
| `s` | **one-line machine-readable health — what scripts gate on** |
| `z` | zero the counters and clear the sticky drop flag |
| `v` | toggle per-report logging of forwarded host→device traffic |
| `U` | put the **RP2040** into BOOTSEL, so the proxy can be reflashed |
| `?` | help |

`s` is the scripting interface; `i` is for people and its layout is not stable
enough to parse:

```
[health] core1=ok drops=0 dropflag=0 sent=213 in=947 indrops=0 \
         piotimeouts=0 cloneerr=0 itf=4 mounted=1 vid=1D50 pid=60FC
```

Every field is something that, if wrong, means bytes did not arrive.
`tools/halfkay_flash.py` reads this between batches and aborts on the spot.

`!` followed by a digit injects a fault — see *fault injection* below. Two keys
so a stray console byte cannot fire one.

The console disappears for ~2 s during any power transition, because the whole
USB device re-enumerates. It always comes back. **Its COM number changes** with
the key's mode, since the proxy adopts the key's VID/PID: look it up, never
cache it. And only one process may hold it at a time.

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

"Needs attention" means: core 1 wedged or the host stack never started; a
PC→device report was dropped after we had already ACKed it; a key is attached but
an interface failed to clone; or the PC has not mounted the proxy. A fault must
persist 2 s before it lights (`PROXY_LED_FAULT_MS`) — enumeration and power
cycles pass briefly through states that look faulty. A real fault outranks the
heartbeat.

The dropped-report fault is **sticky until reset**, and deliberately so: the
sender was told that data was delivered, and there is no way left to correct
that. It must not scroll off the log. `i` says so too.

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

**It fails fast.** Flashing through the proxy means a successful
`HidD_SetOutputReport` proves nothing: the proxy ACKs the control transfer before
it knows whether it can forward the report, so a wedged host core makes every
write "succeed" while the key receives nothing. The flasher therefore watches the
proxy's own `s` health line over the console — auto-detected as the CDC sharing
the key's VID/PID, which a real key does not have — refuses to start if `core1`
is not `ok`, checks every 16 blocks, and **aborts at the failure** rather than
finishing and reporting a success it cannot substantiate.

Exit codes: `0` success, `1` flash error, `2` proxy fault (data was lost). Use
`--no-health` for a directly-attached key.

The official Teensy Loader GUI works through the proxy too; the CLI tool just
makes it repeatable.

---

## Tooling

Everything is scriptable, because the rig is meant to run unattended. If a
procedure needs someone to press something, that is a gap to close.

| Tool | What it does |
|---|---|
| [`selftest.py`](tools/selftest.py) | 12 scenarios, fail-fast, prints the `i` block at the failure |
| [`soak.py`](tools/soak.py) | Bootloader ↔ application cycling, full flash each cycle |
| [`halfkay_flash.py`](tools/halfkay_flash.py) | Flash the key, with proxy health monitoring |
| [`okprobe.py`](tools/okprobe.py) | Ask the key a real question and check it answers |
| [`console.py`](tools/console.py) | The one sanctioned way to hold the console |
| [`touch1200.py`](tools/touch1200.py) | Put the **Feather** into BOOTSEL — no button press |
| [`rig.py`](tools/rig.py) / [`winhid.py`](tools/winhid.py) | Shared console and HID plumbing |

Two rules the helpers exist to enforce. **Only one process may hold the console**
— a second opener gets `PermissionError(13)` and the capture is lost, which cost
two runs. And **the console's COM number moves** when the key changes mode, since
the proxy adopts its VID/PID and re-enumerates, so it is always looked up, never
cached.

`winhid.py` reads with overlapped I/O and a real timeout. A blocking `ReadFile`
on a device that never sends anything waits forever, and a harness that can hang
defeats the point of the exercise.

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

#### `PROXY_OUT_BLOCK_MS` must outlast `PROXY_HALFKAY_ERASE_MS`

There is a `static_assert` on this, because getting it wrong silently corrupts
firmware and the two constants live in different parts of the file.

The erase hold-off deliberately stops `forward_out()` draining for 3 s. A
flashing tool that does not wait for the chip erase — the Teensy Loader GUI does
not — keeps sending at **~35 blocks/s** throughout. If the back-pressure wait is
shorter than the hold-off, those blocks fill the queue and are then **dropped**,
after the PC has already been ACKed.

Measured, with the drop diagnostics that made it visible in one run:

```
11.319  [out] slot 0 block 0 sent, holding 3000 ms for chip erase
12.886  [out] DROPPED itf 0 id=0 type=2 len=1088 : 00 40 00 00
14.328  [out] slot 0 erase window over, resuming
```

A firmware block at `0x004000`, lost inside the hold-off. The hold-off was added
to *prevent* corruption and was causing it.

A deeper queue cannot fix this — 3 s at that rate is ~105 blocks, over 120 KB of
slots. Blocking the sender is the only mechanism that works, and it is exactly
what `teensy_loader_cli` does with its own `sleep(3)`. Across three runs of the
same GUI flash:

| Queue / wait | Drops | Firmware after reboot |
|---|---|---|
| 8 / 250 ms | 22 | dead |
| 16 / 1500 ms | 1 | alive, but only because the lost report was not a block |
| 16 / **4000 ms** | **0** | **alive** |

Verified: 20 consecutive raw-HID writes all forwarded with a reply for each, and
a full firmware flash at `PC->dev : 210 sent, 0 dropped`.

If you touch the OUT path, check `PC->dev` after both a flash *and* a burst of
raw-HID traffic. The two exercise opposite halves of this rule.

The wait also gives up early if core 1 stops ticking. Spinning the full 250 ms
per report for a core that has wedged just delays the watchdog, which needs core
0 running in order to notice.

### The core 1 watchdog

Core 1 runs the host stack, so when it wedges `forward_out()` stops — but core 0
keeps answering the PC perfectly. Every report then hits a queue nobody drains,
gets dropped, and was *already ACKed*. The result is a flashing tool that writes
all 210 blocks, reports success, sends its reboot, logs `IMG_REBOOT_OK`, and the
key receives **none of it**.

This was observed directly. The OnlyKey GUI appeared broken for days while the
CLI worked; the difference turned out to be nothing about the two tools, but
whether core 1 happened to be alive when the run started. Captured with the key
in HalfKay and core 1 wedged:

```
core1     : STALLED, 64635450 ticks, last phase: USBHost.task
PC->dev   : 212 sent, 164 dropped     <- and still climbing
```

The wedge itself is inside Pico-PIO-USB, which waits on PIO hardware flags with
no timeout of its own:

```c
while ((pp->pio_usb_tx->irq & IRQ_TX_EOP_MASK) == 0) { continue; }
while (!(pp->pio_usb_tx->fdebug & stall_mask))      { continue; }
```

Nothing in the library recovers from that, and core 0 cannot fix it from
outside — core 1 is never coming back to be told anything, and its state
machines and DMA channels are left mid-transfer. So if core 1 makes no progress
for `PROXY_CORE1_WATCHDOG_MS` (3 s), core 0 resets the board.

Resetting is the point. It converts silent corruption into an honest error: our
device drops off the bus mid-transfer, so the tool fails loudly instead of
lying. Descriptor dumps hold core 1 in synchronous transfers legitimately, so
they disarm it.

### The HalfKay chip-erase window

Writing HalfKay's block 0 triggers a full chip erase taking seconds, during
which the bootloader NAKs everything. `teensy_loader_cli` and
`tools/halfkay_flash.py` both sleep 3 s there — but **a proxy cannot rely on the
PC doing that**, because the PC is talking to us, not to the key.

Measured with the OnlyKey GUI, which sends block 1 immediately:

```
block 0  x1
block 1  x138 retries
block 2  x181 retries     ~390 control transfers into a busy device
block 3  x73  retries
block 4  x1               erase done, clean from here on
```

The retry logic absorbed it and the flash still completed correctly, but that
burst is the most plausible trigger for the PIO desync above. So the proxy now
enforces the delay itself: after forwarding a block-0 write to a bootloader
interface, nothing else goes out for `PROXY_HALFKAY_ERASE_MS`. Every flashing
tool then behaves, whether or not it waits.

Only the bootloader's own interface qualifies (`known < 0`) and only for
firmware-sized reports — an ordinary 64-byte report beginning `00 00 00` is
commonplace and must not stall the queue for three seconds. The reboot command
is `FF FF FF`, so it is never mistaken for this.

### Fault injection

Every safety mechanism here exists for a failure that is rare, hard to provoke,
and catastrophic when missed. That combination means they are almost never
exercised: a 25-cycle soak and 14 scenarios fired the watchdog **zero** times.
Every green run proved the happy path and said nothing about whether the
recovery paths work at all. An untested recovery path is not a recovery path.

So each one can be triggered on demand. Two keys (`!` then a digit), so a stray
byte on the console cannot fire one — single characters there already
power-cycle the port and reboot the board.

| Key | Injects | Proves |
|---|---|---|
| `!1` | Core 1 stops ticking | The watchdog resets the board and it recovers |
| `!2` | A discarded report | Sticky flag, LED, status warning, `z` clears it |
| `!3` | PIO timeout in 6 s | Lands mid-flash — the flasher must refuse the image |
| `!4` | A failed interface clone | `cloneerr`, LED, and that a detach clears it |
| `!5` | A dropped report in 6 s | Lands mid-flash — the flasher must abort |

`!1` stops core 1's tick counter in `loop1()`, which is the same signature as a
real hang inside `USBHost.task()` — core 0 still running, still answering the
PC. That is what made the original bug so expensive, and it is now one
keystroke.

`!3` and `!5` are deferred by 6 s on purpose: the flasher holds the console for
its own health checks while it runs, so there is no way to inject during a flash
from outside. Arming first and letting the fault land mid-transfer is the only
way to prove those gates fire.

Note the difference between `!2` and `!5`. A drop flag left over from an
**earlier** run must not block a new flash — the flasher zeroes the counters at
start, which is the documented way to begin a clean run. What must abort a flash
is a drop **during** it.

Measured: `!1` wedged core 1 and the board reset 2.6 s later with counters
zeroed and all four interfaces back.

### IN endpoint arming

Interfaces are polled only after `PROXY_ARM_DELAY_MS` (500 ms), so a device that
has just enumerated is not hit with an IN request while still initialising.

`arm_service()` keeps a request outstanding at all times rather than arming
once. An earlier version armed a single time per mount and relied on the report
callback to re-arm; a single failed re-arm silenced that interface permanently,
which stalled forwarding after a few hundred reports. `tuh_hid_receive_ready()`
is false while a request is pending, so this only re-issues once the previous
report has actually arrived.

**The `rearm` counter is not a fault signal.** It once was, back when the report
callback did the re-arming and `arm_service()` only stepped in after a broken
chain. Now that `arm_service()` is the sole armer it increments on every normal
re-arm, so it tracks `in` almost exactly — 2502 in / 2502 rearm on a keyboard
during a backup is healthy, not alarming. `indrops` is the signal to watch.

**Only `arm_service()` may arm an interface**, and it refuses unless a queue
slot is free:

```c
if (!g_inq[i].reserve()) continue;   // don't ask for data we cannot store
```

That is the whole flow-control guarantee. An **unrequested** report stays in the
device — real USB back-pressure — whereas a **received** one can only be
dropped.

`tuh_hid_report_received_cb()` used to end with an unconditional
`tuh_hid_receive_report()`, which defeated that guarantee one line later: it
asked for the next report regardless of queue space, so a queue that could not
drain (the PC detached mid-transition, say) overflowed. Measured at **12 lost
device→PC reports across a single test run**, on the FIDO2 and raw-HID path
among others.

The comment defending it — "re-arm immediately or the interface goes silent" —
was true when `arm_service()` armed once per mount. It stopped being true when
that changed, and the now-harmful line outlived its justification. `indrops` in
the health line is the check; it must be zero.

### Entering the key's bootloader

Pulling the bootloader contact to ground for ~300 ms reboots a **running** key
straight into HalfKay. That is what `b` does, and it is the normal route:

```
[bsel] contact held low for 300 ms
[host] device detached: addr 1
[host] itf 0 (keyboard) unmounted        ... and the rest
```

The proxy then follows the key automatically. Observed from the PC side, the
whole swap takes about a second:

```
15:39:02  VID_1D50&PID_60FC   app mode, 4 HID interfaces + console
15:39:58  (nothing)           key entered bootloader, proxy detached
15:39:59  VID_16C0&PID_0478   HalfKay: MI_00 = HID, MI_01 = console
```

No reset, no power cycle, no intervention: the key swaps its descriptors, core 1
sees the unmounts, the new descriptor is cloned, the configuration is rebuilt
around it and the VID/PID is adopted. Interface numbering adapts too — HalfKay's
single HID lands at MI_00, so a flashing tool sees the same layout as a
directly-attached key.

#### Recovering a key with no firmware — the `B` cold entry

**The bootloader triggers on RELEASE, not while the contact is held.** It is a
button press: ground it briefly, then let go. Holding the contact down *prevents*
the trigger, so "hold it until something happens" is exactly wrong — an attempt
built on that assumption produced a key that had left application mode and then
offered no interfaces at all.

The other half: `b` works on a running key because the *application firmware*
notices the press. Entering the bootloader invalidates that firmware, so a key
left stuck **descriptor-less** — enumerated, correct VID/PID, console present,
but `itf=0` — has nothing running to notice anything. It has to be power-cycled
first so something is there to press.

So the working sequence, which is just what a human does, is:

1. Cut VBUS for `PROXY_BOOTSEL_COLD_OFF_MS` (3 s)
2. Restore VBUS and **let the key boot** — `PROXY_BOOTSEL_COLD_BOOT_MS` (1.5 s)
3. Press the contact for `PROXY_BOOTSEL_COLD_HOLD_MS` (300 ms)
4. **Release** — this is the trigger

That is `B`. Use it, not `b`, whenever the port has gone silent.

##### Why `B` sometimes is not enough — and what is NOT the reason

**VBUS cycling does power the key down.** Observed directly: during a `p` cycle
the OnlyKey runs its LED boot sequence, which is the key itself reporting a cold
start. Any claim that the rail stays alive is wrong.

An earlier version of this section asserted the opposite — that the PIO driving
D+/D− back-feeds the key through its protection diodes and keeps it powered.
That was **inference, not measurement**, built to explain why `R` recovered a
stuck key when `B` did not. It was written up as established fact and it is not
true. A long-standing note elsewhere in this project claiming the VBUS commands
"do not make the key re-run its startup" is wrong for the same reason.

So `PROXY_CUT_DATA_LINES` is **not** needed for power cycling, and the argument
that made it look necessary does not hold.

**GPIO 6's reset pull-down does not trigger the bootloader either.** During
`coldcycle.py` the RP2040 sits in its ROM bootloader with every pad at reset
state, so the contact is grounded through the key's *entire* power-up — and the
key came back in **application** mode. So "contact grounded across power-up
enters the bootloader" is out as well.

What remains genuinely unexplained: on a key stuck **descriptor-less**, three
automatic cold entries, three manual `b` presses and repeated VBUS cycles did
nothing, while `R` recovered it immediately every time. Both power-cycle the
key. Neither the data lines nor pull-down timing explains the difference —
every mechanism proposed so far has been tested and eliminated.

The honest state of this is **no working theory**. Until there is one, automatic
recovery escalates to an RP2040 reset after `PROXY_BOOTSEL_RESET_AFTER` (2)
failed cold entries, purely because a reset is measured to work every time and
not because anyone knows why.

The recovery also **no longer gives up** after a fixed number of tries —
stopping left the rig permanently stranded needing a human, which is the
opposite of the point. It backs off instead, up to
`PROXY_BOOTSEL_BACKOFF_MAX_MS` (2 min) between attempts, because retrying every
few seconds against an empty port re-enumerates the whole device each time and
makes the console unusable.

##### When this actually happens

Worth being precise, because it is easy to over-read: on a **healthy** key,
grounding the contact from application mode goes straight to HalfKay in about
1.6 s. Verified both by hand and scripted:

```
0.834  --- sent 'b' ---
0.885  [bsel] contact held low for 300 ms
0.885  [host] device detached: addr 1
0.885  [host] itf 0..3 unmounted
0.885  [bsel] contact released
2.474  --- attached to COM15 ---     16C0:0478, HID bound
```

The descriptor-less dead end only appeared when the key's firmware was already
damaged — after a flash that the `piotimeouts` gate had correctly refused. Which
is exactly the moment recovery matters most, so the handling above earns its
place even though the common path never touches it.

> **A cold entry blocks core 1 for several seconds**, which is far longer than
> `PROXY_CORE1_WATCHDOG_MS`. It sets `g_core1_long_op` so the watchdog does not
> mistake deliberate work for a wedge. Without that the board reset-loops
> forever: the reset restores the very defaults that start the recovery. This
> happened, and it takes the console down on every cycle, so it is unpleasant to
> diagnose from the outside. Any new operation that blocks core 1 past the
> watchdog must set the same flag.

Note on the VBUS commands: `p` / `0` / `1` alone do not reach the bootloader —
they only power-cycle. The press-and-release afterwards is what triggers it, and
that is what `B` adds. `0` also leaves VBUS **off until something turns it back
on**; a status line reading `nothing attached, or no VBUS` may simply mean the
rail is switched off, so check `5V_EN` in `h` before concluding the key is dead.

### The exact flashing procedure

```bash
# 1. put the key into its bootloader
#    press 'b' in the console; verify the port reappears as 16C0:0478

# 2. flash
python tools/halfkay_flash.py firmware.hex

# 3. verify
#    the port returns as 1D50:60FC with every interface cloned, and
#    `i` must show BOTH counters reading "N sent, 0 dropped"
```

Step 3's zero-drop check is not optional — see *back-pressure* below. A drop on
`PC->dev` during a flash means a corrupted image that will not boot.

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
| `PROXY_OUT_QUEUE_LEN` | `16` | Host→device queue depth. Too shallow silently corrupts firmware writes |
| `PROXY_OUT_BLOCK_MS` | `4000` | How long a large report waits for a slot. **Must exceed `PROXY_HALFKAY_ERASE_MS`** — see *back-pressure* |
| `PROXY_SMALL_REPORT_MAX` | `64` | At or below this, a report never blocks the IRQ |
| `PROXY_IN_QUEUE_LEN` | `32` | Device→PC queue depth per interface |
| `PROXY_BOOTSEL_REBOOT_GRACE_MS` | `30000` | Silence tolerated before poking a key that has already enumerated once |
| `PROXY_ARM_DELAY_MS` | `500` | Settle time before first polling an interface |
| `PROXY_POWER_OFF_MS` | `750` | Off duration for `p` |
| `PROXY_MAX_HID` | `4` | Must be ≤ `CFG_TUD_HID` in `platformio.ini` |
| `PROXY_PIXEL_BRIGHTNESS` | `40` | NeoPixel ceiling, 0–255 |
| `PROXY_LED_FAULT_MS` | `2000` | How long a fault must persist before GPIO 13 lights |
| `PROXY_CORE1_WATCHDOG_MS` | `3000` | Core 1 silence tolerated before core 0 resets the board |
| `PROXY_HALFKAY_ERASE_MS` | `3000` | Hold-off after a HalfKay block-0 write, for the chip erase |
| `PROXY_FAULT_DELAY_MS` | `6000` | How long `!3` / `!5` wait, so the fault lands mid-flash |
| `PROXY_BOOTSEL_COLD_OFF_MS` | `3000` | VBUS off during a cold entry |
| `PROXY_BOOTSEL_COLD_BOOT_MS` | `1500` | Let the key boot before pressing the contact |
| `PROXY_BOOTSEL_COLD_HOLD_MS` | `300` | Press duration — release is what triggers the bootloader |
| `PROXY_BOOTSEL_BACKOFF_MAX_MS` | `120000` | Longest gap between automatic recovery attempts |
| `PROXY_BOOTSEL_RESET_AFTER` | `2` | Failed cold entries before escalating to an RP2040 reset |
| `PROXY_CUT_DATA_LINES` | `0` | Unproven experiment — **known to wedge the host stack** |

---

## Known issues

**Entering the OnlyKey's bootloader invalidates its application firmware.** You
must re-flash; there is no booting back out. `halfkay_flash.py --boot-only`
returns the key to *HalfKay*, not to the application, because HalfKay has nothing
valid to jump to. This is correct Teensy behaviour, but it looks exactly like a
passthrough failure and has been misread as one more than once — including as
apparent evidence against a fix that was working correctly. Any procedure that
enters the bootloader must flash to get back.

Flashing or resetting the **Feather** power-cycles the key, so where it lands
depends entirely on whether its firmware is valid:

- **valid firmware** → cold-boots straight back into **application** mode
- **invalidated** (an earlier bootloader entry) → **bootloader**, because there
  is nowhere else to go

Both observed with the same tool minutes apart. An earlier version of this note
claimed a Feather reset *always* leaves the key in the bootloader, blaming
GPIO 6's reset pull-down for grounding the contact through the power-up. That
was a guess; firmware validity explains every observation without it.

**VBUS commands ARE a true power cycle.** This entry previously said the
opposite — that cutting VBUS did not make the key re-run its startup, mechanism
unknown. That is **disproved**, by two independent measurements:

- With VBUS off, all four interfaces unmount and the key sends **nothing** for
  15 s, on a device that otherwise chatters constantly. A powered device holds
  its D+ pull-up and stays enumerated; this one drops off the bus entirely.
- On `p`, the OnlyKey runs its **LED boot sequence** — the key itself reporting
  a cold start.

The first of those also **disproves the D+/D− back-feed theory directly**, not
merely by inference: the proxy is still running throughout a `0`, so the PIO is
still driving the data lines. VBUS off, data lines driven, key dead. That is the
exact condition under which back-feed would have to show itself, and it does
not.

Both checks take under a minute. The old claim survived a long time only because
nobody ran them.

`PROXY_CUT_DATA_LINES=1` releases the data pads during power-off, on that same
theory that leakage keeps the device alive. **The theory is dead, so the option
has no known purpose.** It remains off by default and also wedges Pico-PIO-USB,
because releasing the pads mid-flight stops the host stack servicing and the
command that would restore power never runs. Treat it as vestigial rather than
as pending work.

**Core 1 stalls — both causes now addressed.**

One cause was a call to `tuh_hid_itf_get_total_count()`, which walks the HID host
driver's interface table from outside the normal task flow — while the driver was
tearing those very entries down during an application ↔ bootloader swap. It only
fed a cosmetic counter, so the count now comes from our own `g_link[]`.

The other was inside Pico-PIO-USB: unbounded spins on PIO hardware flags, running
in the **SOF alarm interrupt**. When a state machine desyncs — which a device
detaching mid-transfer can cause — the flag never arrives, the SOF callback never
returns, and everything waiting on it (`abort_transfer`, `pio_usb_host_stop`, the
bus itself) hangs with it. The library is now vendored at `lib/Pico_PIO_USB/`
with every such wait bounded; see `lib/Pico_PIO_USB/PATCHES.md`.

`pio_usb_tx_timeouts` counts every give-up and appears in `i` and the `s` health
line, so a silent recovery is still a reported one.

The watchdog remains as a backstop. These are belt and braces, not alternatives:
the patches remove the known hang, the watchdog covers the ones we have not met
yet. Verified with `tools/soak.py`, which drives bootloader ↔ application cycles
with a full flash each time.

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
- **Live descriptor swap**: `b` reboots a running key into HalfKay and the proxy
  follows it in ~1 s with no reset, verified from the Windows device list
- Power control by console and BOOT button; console-only re-enumeration
- **The OnlyKey desktop GUI flashing and rebooting the key through the proxy** —
  210 blocks, `PC->dev : 210 sent, 0 dropped`, reboot forwarded, key returns as
  `1D50:60FC` with all four interfaces. GUI and CLI reboots captured side by side
  and are byte-identical (`id=0 type=2 len=1088`, `FF FF FF 00…`)
- **The erase hold-off** — retries across the chip-erase window went from ~390 to
  zero, one log line per delivered report
- **25 consecutive bootloader ↔ application cycles** under `tools/soak.py` in
  564 s — 50 mode transitions, ~5,250 blocks, ending `drops=0 indrops=0 itf=4`
- **The key's firmware answering through the proxy**: `okprobe.py` gets
  `Error OnlyKey must be initialized first` back over raw HID at MI_02
  (usage page 0xFFAB) — the key's own firmware, so TX and RX are both proven
- **Grounding the bootloader contact by hand**, from application mode on a
  healthy key: straight to `16C0:0478` with the HID interface bound, in ~1.6 s,
  no descriptor-less state. Confirmed both physically and scripted
- **The `piotimeouts` gate refusing a real flash.** A genuine desync at block 32
  aborted the run with *"a block may not have reached the key even though nothing
  was dropped — this image cannot be trusted. Re-flash."* Before this gate that
  run would have written a corrupt image and reported success
- Clean build under `-Wall -Wextra` across all three environments

- **The PIO bounded waits catching a real desync.** During a 25-cycle soak the
  counter went to `piotimeouts=1` on cycle 4 and that cycle still passed — the
  bus desynced, the wait gave up, the transaction was abandoned and the stack
  carried on. Under the original unbounded spin the same event hangs core 1
  permanently. This is the failure that used to corrupt firmware flashes
  silently.
- **Every safety mechanism fired by fault injection**, not inferred: `!1` wedged
  core 1 and the watchdog reset the board in 2.6 s; `!2` set the sticky drop flag
  and the status warning; `!5` aborted a flash mid-run; `!3` made the flasher
  refuse an image; `!4` raised `cloneerr`

- **The OnlyKey working as an OnlyKey, through the proxy, with its own software.**
  All four interfaces exercised by real applications in one session — a slot
  label configured in the OnlyKey app (raw HID), an encrypted backup typed into
  Notepad as **2502 keystrokes** (keyboard), and a **WebAuthn login at
  webauthn.io** (FIDO2) — ending:

  ```
  slot 0 keyboard: 2502 in     slot 1 fido2:  68 in
  slot 2 rawhid:     97 in     slot 3 seremu: 2233 in
  dev->PC : 6034 sent, 0 dropped
  PC->dev :  272 sent, 0 dropped
  ```

  This is the evidence counters alone cannot give: the host does not just
  receive the bytes, it *interprets* them correctly — a browser completed a
  real WebAuthn ceremony and an app read back what it wrote.

**Not yet verified:**

- **The watchdog firing on a real wedge.** Still no wedge to hand — which is now
  the point, since the known cause is fixed. It remains a backstop for hangs
  that have not been met yet.

**Notes on reading the counters:** `dev->PC` sitting still is not necessarily a
fault — the keyboard, FIDO2 and raw HID interfaces only send on user action, and
seremu goes quiet once it has flushed its startup output.

`rearm` is **not** a fault signal (it once was; see *IN endpoint arming*). It now
tracks `in` almost exactly because `arm_service()` is the sole armer. The
signals that matter are `drops`, `indrops`, `dropflag`, `cloneerr` and a rising
`piotimeouts`.
