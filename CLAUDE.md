# Working on this repo

A USB HID passthrough on an Adafruit Feather RP2040 with USB Type-A Host. An
OnlyKey plugs into the host port; the Feather clones it to the PC and forwards
reports both ways.

**The point of the project is an unattended rig** — an LLM flashing and
exercising OnlyKey firmware without a human pressing buttons. Every step below
is scriptable. If you find yourself about to ask the user to press something,
that is a bug in the procedure, not a request to make.

---

## This machine

There is **no system Python** (`python` hits a Store stub, exit 49) and **`pio`
is not on PATH**. Use the PlatformIO venv:

```powershell
$PY  = "C:\Users\bmatu\.platformio\penv\Scripts\python.exe"      # has pyserial
$PIO = "C:\Users\bmatu\.platformio\penv\Scripts\platformio.exe"
```

## Build

```powershell
& $PIO run                                          # all three environments
& $PIO run -e adafruit_feather_rp2040_usb_host      # the real one
```

`diag` and `diag_trace` are diagnostic builds. All three must compile.

## Flash the Feather — no hands required

```powershell
& $PY tools\touch1200.py COM14        # 1200-baud DTR touch -> RPI-RP2 on D:
& $PIO run -e adafruit_feather_rp2040_usb_host -t upload
```

`Adafruit_USBD_CDC`'s `tud_cdc_line_state_cb()` enters DFU when DTR drops at
1200 bps, so this replaces double-tapping reset. `upload_command` copies the
UF2 to `D:\`. The console command `U` does the same thing from the other side.

## Talk to the proxy

```powershell
& $PY tools\console.py                  # auto-detect, print the health line
& $PY tools\console.py --keys i         # status
& $PY tools\console.py --keys b         # put the KEY into its bootloader
& $PY tools\console.py --watch 60       # log for 60 s
```

The console is the Feather's CDC. **Its COM number changes** when the key
switches mode, because the proxy adopts the key's VID/PID and re-enumerates —
roughly COM14 in application mode (`1D50:60FC`), COM15 in bootloader mode
(`16C0:0478`). `console.py` auto-detects for that reason; never hardcode.

**Only one process may hold the port.** A second opener gets `PermissionError
(13)` and the capture is lost. Do not leave `--watch` running during a flash —
`halfkay_flash.py` opens the console itself for health checks.

Key commands: `i` status · `s` health line · `z` zero counters · `b` put the
**key** into its bootloader · `p`/`0`/`1` power · `v` verbose OUT · `U` Feather
BOOTSEL · `?` help.

## Flash the OnlyKey through the proxy

```powershell
& $PY tools\halfkay_flash.py firmware.hex     # finds the console, fails fast
```

It auto-detects the proxy console (the CDC sharing the key's VID/PID — a real
key has none, which is how it tells them apart), zeroes counters, refuses to
start if `core1` isn't `ok`, checks every 16 blocks, and aborts **at** the
failure. Exit `0` success, `1` flash error, `2` proxy fault (data was lost).

---

## Quirks that will waste your time

**Entering the OnlyKey's bootloader invalidates its application firmware. You
must re-flash — there is no booting back out.** `halfkay_flash.py --boot-only`
returns the key to *HalfKay*, not to the application, because HalfKay has
nothing valid to jump to. This is correct Teensy behaviour and looks exactly
like a passthrough failure; it has been misread as one more than once. Any test
that enters the bootloader must flash to get back. `tools/soak.py` flashes every
cycle for this reason.

**Flashing the Feather leaves the key in the bootloader.** The RP2040 reset
drops VBUS and reverts GPIO 6 to input-with-pull-down, so the key restarts with
its contact grounded. Expect to re-flash the key after every proxy upload.

**A VBUS power cycle is not a true cold start.** `p`/`0`/`1` drop the key off
the bus but do not make it re-run its startup. Only a physical unplug does. The
mechanism is unconfirmed — treat it as an observation, not a theory.

**`v` is a toggle and it survives re-enumeration.** The RP2040 does not reset
when the proxy re-enumerates, so verbose logging stays as it was between runs.
Sending `v` blindly turns it *off* half the time — read the acknowledgement back
and only toggle again if it says `off`. This cost one capture.

**Counters persist across key mode changes**, since only an RP2040 reset or `z`
clears them. A `sent=212` after a mode switch may belong to the *previous* run;
zero them first or you will attribute one run's numbers to another.

**The `[out]` log prints on delivery, not per attempt.** Retries are normal
while the control pipe is busy and are not logged, so line count equals reports
delivered.

## Hard rules

- **Only core 1 calls `tuh_*`; only core 0 calls `tud_*`/`TinyUSBDevice`.**
  Crossing this wedges the stack.
- **240 MHz is mandatory.** At 120 MHz enumeration stalls at SET_ADDRESS.
  `board_build.f_cpu = 240000000L`.
- **`lib/Adafruit_TinyUSB_Arduino/` is a vendored, patched copy.** Three
  `// PATCHED` sites; do not replace it with the upstream library.
- **Never log directly to a port.** Everything goes through `LogRing`; core 0
  drains it. Writing to CDC from an IRQ deadlocks the device stack.
- **A dropped OUT report is silent corruption**, because the host was ACKed
  before we knew we could forward it. Never make a drop quieter.

## Verifying a change

```powershell
& $PY tools\soak.py 25     # bootloader <-> application, full flash each cycle
```

Drives the window the host stack used to wedge in — detach and re-enumeration —
and checks `core1`, `drops` and `piotimeouts` after every cycle. Stops at the
first failure with the cycle number and the phase core 1 died in. ~20 s/cycle.

`PC->dev` and `dev->PC` must both read `N sent, 0 dropped` after **both** a
firmware flash *and* a burst of raw-HID traffic. Those exercise opposite halves
of the back-pressure rule and a change can easily fix one while breaking the
other.

## The core 1 hang

Pico-PIO-USB waited on PIO hardware flags with no timeout, inside the SOF alarm
interrupt. A detach mid-transfer can desync a state machine, the flag never
arrives, and the callback never returns — taking `abort_transfer()`,
`pio_usb_host_stop()` and the whole bus with it. Core 1 froze in
`USBHost.task()` while core 0 kept ACKing PC writes it could no longer forward.

The library is vendored at `lib/Pico_PIO_USB/` with every such wait bounded —
see `lib/Pico_PIO_USB/PATCHES.md`. **It is not in `lib_deps`; adding it back
installs a pristine copy that silently reverts the patches.**
`pio_usb_tx_timeouts` (in `i` and `s`) counts every give-up.

Confirmed working: a 25-cycle soak recorded `piotimeouts=1` on cycle 4 and that
cycle still passed — a real desync, caught and recovered instead of hanging. A
non-zero count is normal and survivable; it is reported rather than swallowed
because a silent recovery would just be a quieter version of the original bug.

The watchdog stays as a backstop for hangs we have not met yet.

See `README.md` for the full design rationale and the failure history.
