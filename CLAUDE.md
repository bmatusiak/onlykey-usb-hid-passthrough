# Working on this repo

**[WORKFLOWS.md](WORKFLOWS.md) holds the verified procedures** — build, flash,
recover, test, capture, inject faults. Every command in it has been run
successfully on this rig. Start there for "how do I do X"; this file is the
rules and context behind them.

**Record only true statements** in docs, comments and memory — never a false
claim, not even as a correction. Retractions decay: "an earlier version claimed
X, that was wrong" compacts down to "X". To close off a dead theory, state the
measurement that rules it out, phrased so it stays true if the framing is
stripped.

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

**Fault injection** (`!` then a digit — two keys so a stray byte cannot fire
one). These exist because the safety mechanisms are otherwise never exercised:
25 soak cycles and 14 scenarios fired the watchdog zero times, so a green run
proved only the happy path.

| Key | Injects | Proves |
|---|---|---|
| `!1` | Core 1 stops ticking | Watchdog resets the board, recovers |
| `!2` | A discarded report | Sticky flag, LED, status warning |
| `!3` | PIO timeout in 6 s | Mid-flash — flasher refuses the image |
| `!4` | A failed interface clone | `cloneerr`, LED |
| `!5` | A dropped report in 6 s | Mid-flash — flasher aborts |

`!3`/`!5` are deferred because the flasher holds the console while it runs, so
there is no way to inject during a flash from outside. A stale drop flag must
NOT block a new flash (the flasher zeroes counters at start); only a drop
*during* a flash aborts it.

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

**The bootloader triggers on RELEASE of the contact, not while it is held.** It
is a button press: ground briefly, then let go. Holding it down *prevents* the
trigger — a fix built on "hold it through enumeration" made things worse.

**And `b` only works on a key that is running firmware**, because the
application is what notices the press. A stuck descriptor-less key (enumerated,
right VID/PID, console present, but `itf=0`) has nothing running, so it must be
power-cycled first. Use **`B`**: cut VBUS 3 s → restore → let it boot 1.5 s →
press 300 ms → **release**. `R` works too, since a reset power-cycles the key.

**Cutting VBUS IS a real power cycle** — measured twice: with VBUS off the key
unmounts and sends nothing for 15 s, and on `p` it runs its LED boot sequence.
The proxy keeps running during a VBUS-off, so the data lines are still driven
and the key dies anyway — they do not sustain it. `PROXY_CUT_DATA_LINES` is
vestigial; it serves no known purpose.

**GPIO 6's reset pull-down does not enter the bootloader.** In RP2040 BOOTSEL
every pad sits at reset state through the key's whole power-up, and the key
returns in application mode.

Still unexplained: on a **descriptor-less** key, `R` recovers it every time and
`B` does not, even though both power-cycle it. There is **no working theory** —
power, data lines and pull-down timing are all ruled out above. Do not spend
time re-deriving those three.

Auto-recovery does the cold entry, backs off to 2 min rather than giving up
(stranding the rig needs a human, which defeats the point), **escalates to an
RP2040 reset after `PROXY_BOOTSEL_RESET_AFTER` (2) failures** because that is the
recovery measured to work, and can be switched off with `A` when poking at the
board by hand.

On a **healthy** key none of this is needed: grounding the contact from
application mode reaches HalfKay in ~1.6 s. The dead end only appears when the
firmware is already damaged — i.e. after a refused flash, which is exactly when
recovery matters.

**A cold entry blocks core 1 for ~5 s, far longer than the 3 s watchdog**, so it
sets `g_core1_long_op`. Without that the board reset-loops forever — the reset
restores the defaults that start the recovery. Any new operation that blocks
core 1 past the watchdog must set the same flag.

**`0` leaves VBUS off until something turns it back on.** `nothing attached, or
no VBUS` in the status often means the rail is simply switched off — check
`5V_EN` in `h` before concluding the key is dead.

**Entering the OnlyKey's bootloader invalidates its application firmware. You
must re-flash — there is no booting back out.** `halfkay_flash.py --boot-only`
returns the key to *HalfKay*, not to the application, because HalfKay has
nothing valid to jump to. This is correct Teensy behaviour and looks exactly
like a passthrough failure; it has been misread as one more than once. Any test
that enters the bootloader must flash to get back. `tools/soak.py` flashes every
cycle for this reason.

**A Feather reset may leave the key in the bootloader — but only if the key has
no valid firmware.** A key with valid firmware cold-boots straight back into
application mode; a key whose firmware was invalidated by an earlier bootloader
entry has nowhere else to go. Both observed with the same tool minutes apart.

**A VBUS power cycle IS a true cold start.** `p` makes the key re-run its
startup — its LED boot sequence runs, and it emits a fresh startup banner on
seremu (362 reports from zeroed counters). `coldcycle.py` does the same via the
RP2040 ROM bootloader, where every pad is at reset state and the key is provably
off the bus.

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

**`piotimeouts` is a correctness signal, not a curiosity.** A timeout means a
bounded wait in Pico-PIO-USB gave up and **abandoned** the transaction, so the
data may never have reached the key — while `sent` already counted it. Zero
drops is therefore *not* sufficient proof of a good flash: a run can report
`210 sent, 0 dropped` and still write a corrupt image. `halfkay_flash.py`
baselines this counter and exits 2 if it moves. Retry the flash; do not
carry on. (`z` does not clear it — it lives in the USB library, not our
counters.)

**Enumerating is not being ready.** Straight after a flash the key is still
running its startup — seremu visibly streams its banner — and raw HID does not
answer for a few seconds. Even on a settled key the *first* query after opening
the interface usually gets no reply and the second works. Always retry before
concluding a key is dead; `okprobe.py --settle` does this.

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
- **`PROXY_OUT_BLOCK_MS` must exceed `PROXY_HALFKAY_ERASE_MS`** (there is a
  `static_assert`). The hold-off stops forwarding for 3 s while a GUI that does
  not wait for the chip erase keeps sending at ~35 blocks/s — if the wait is
  shorter, those blocks are dropped and the image is corrupt. A deeper queue
  cannot substitute: 3 s at that rate is ~105 blocks, >120 KB.
- **Only `arm_service()` may arm an IN endpoint**, and only when a queue slot is
  free. An unrequested report stays in the device — real USB back-pressure —
  while a received one can only be dropped. `tuh_hid_report_received_cb()` must
  **not** re-arm: doing so defeated the guarantee and lost 12 device→PC reports
  in one test run. `indrops` must be zero.

## Verifying a change

```powershell
& $PY tools\selftest.py            # 12 scenarios, fail-fast
& $PY tools\selftest.py --list     # what they are
& $PY tools\selftest.py -k flash   # just the flashing ones
& $PY tools\soak.py 25             # stability: 25 bootloader<->app cycles
& $PY tools\okprobe.py --count 20  # raw-HID round trips only
```

`selftest.py` stops at the first failure and prints the `i` block, because the
phase core 1 died in is the most useful thing to know and it is gone the moment
anything else touches the device. Scenarios that enter the bootloader always
flash back, so the rig is left usable.

**The check that actually matters is `flash_then_key_responds`.** HalfKay has no
read-back, so an image cannot be verified directly, and "210 blocks sent" is
exactly the claim that was false while the key received nothing. The honest
substitute is end-to-end: flash, reboot, and require the firmware to boot and
answer over raw HID. An "uninitialized" reply is a pass — that is the key's own
firmware talking.

`rearm` is **not** a fault signal — it tracks `in` almost exactly now that
`arm_service()` is the sole armer. Watch `drops`, `indrops`, `dropflag`,
`cloneerr` and a rising `piotimeouts` instead.

`PC->dev` and `dev->PC` must both read `N sent, 0 dropped` after **both** a
firmware flash *and* a burst of raw-HID traffic. Those exercise opposite halves
of the back-pressure rule and a change can easily fix one while breaking the
other — which is why `hid_round_trip_integrity` and the flash scenarios are
separate tests.

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
