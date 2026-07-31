# Known-working workflows

Procedures that have been run and verified on this rig. Every command here has
been executed successfully; nothing is aspirational.

Paths assume this machine: there is no system Python and no `pio` on PATH, so
both come from the PlatformIO venv.

```powershell
$PY   = "C:\Users\bmatu\.platformio\penv\Scripts\python.exe"
$PIO  = "C:\Users\bmatu\.platformio\penv\Scripts\platformio.exe"
$BASE = "c:\Users\bmatu\Desktop\software\ai stuff\feather\basic"
```

Two rules that apply to everything below:

- **Only one process may hold the console.** A second opener gets
  `PermissionError(13)`. Stop any logger before running a flash — the flasher
  opens the console itself for health checks.
- **The console's COM number changes with the key's mode**, because the proxy
  adopts the key's VID/PID and re-enumerates. Look it up every time.

---

## Build

```powershell
& $PIO run -d $BASE                                      # all three environments
& $PIO run -d $BASE -e adafruit_feather_rp2040_usb_host   # the real one
```

`-d $BASE` matters: the working directory is not always the project root.

## Flash the Feather — no button press

```powershell
$com = [regex]::Match((Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.Name -match 'COM\d+' } | Select-Object -First 1).Name,'COM\d+').Value
& $PY "$BASE\tools\touch1200.py" $com
Start-Sleep -Seconds 5
& $PIO run -d $BASE -e adafruit_feather_rp2040_usb_host -t upload
```

The 1200-baud DTR touch enters BOOTSEL; `upload_command` copies the UF2 to `D:`.
Wait ~10 s afterwards for the proxy to re-enumerate.

If the board is reset-looping and the console keeps vanishing, retry the touch in
a loop until `RPI-RP2` appears — a window always comes up within a few tries.

## Talk to the proxy

```powershell
& $PY "$BASE\tools\console.py"                # auto-detect, print health
& $PY "$BASE\tools\console.py" --keys "i"     # human-readable status
& $PY "$BASE\tools\console.py" --keys "z"     # zero counters before a measurement
& $PY "$BASE\tools\console.py" --watch 60     # log for 60 s
```

`s` (health) is the machine-readable line scripts gate on. `i` is for people.

## Put the key into its bootloader

```powershell
& $PY "$BASE\tools\console.py" --keys "b"     # press/release the contact
Start-Sleep -Seconds 6
```

Works on a key that is **running firmware**. Lands at `16C0:0478` with a HID
interface bound, typically in under 2 s. Grounding the physical contact by hand
does the same thing.

## Flash the key

```powershell
Set-Location $BASE
& $PY "tools\halfkay_flash.py" "firmware.hex"
```

Finds the proxy console itself, zeroes counters, refuses to start if `core1` is
not `ok`, checks every 16 blocks and aborts at the failure.

Exit `0` success, `1` flash error, `2` proxy fault (data lost — **re-flash**,
the image cannot be trusted).

Afterwards the key returns to application mode automatically. Confirm the
firmware actually runs:

```powershell
Set-Location "$BASE\tools"; & $PY okprobe.py
```

An `Error OnlyKey must be initialized first` reply is a **pass** — that is the
key's own firmware answering.

## Cold power cycle

```powershell
& $PY "$BASE\tools\console.py" --keys "p"     # VBUS cycle
```

A genuine cold start: the key runs its LED boot sequence and emits a fresh
startup banner on seremu. Lands in **application** mode when the firmware is
valid.

For a power cycle that does not depend on the proxy firmware running at all:

```powershell
Set-Location "$BASE\tools"; & $PY coldcycle.py --hold 6
```

Routes through the RP2040 ROM bootloader, where every pad is at reset state and
the key is provably off the bus (`key nodes on the bus while unpowered: 0`).
Use this when the proxy itself is wedged.

## Recover a key that has gone silent

In order of escalation. The proxy does this automatically, but by hand:

1. `--keys "b"` — press/release. Works if the key is running firmware.
2. `--keys "B"` — cold entry: VBUS cycle, let it boot, press, release.
3. `--keys "R"` — RP2040 reset. **The recovery measured to work every time** on
   a key stuck descriptor-less (`itf=0`).

A descriptor-less key looks healthy from outside — console present, right
VID/PID, `core1=ok` — and only `itf=0` reveals it. After recovery it will be in
the bootloader, so flash it to get back to application mode.

`--keys "A"` switches automatic recovery off, which is necessary before poking
at the board by hand — otherwise it power-cycles every few seconds and takes the
console with it.

## Test

```powershell
Set-Location "$BASE\tools"
& $PY selftest.py            # 19 scenarios, fail-fast, ~7 min
& $PY selftest.py --list     # what they are
& $PY selftest.py -k flash   # only matching
& $PY soak.py 25             # 25 bootloader<->application cycles, ~10 min
& $PY okprobe.py --count 20  # raw-HID round trips only
```

`selftest.py` stops at the first failure and prints the `i` block. Scenarios that
enter the bootloader flash back, so the rig is left usable.

Run `selftest.py` after any change to the OUT path, the recovery code or the
vendored USB libraries.

## Capture what the PC sends

For diagnosing a flashing tool that is not `halfkay_flash.py`:

```powershell
& $PY "$BASE\tools\console.py" --keys "z"     # zero first
# then, as the ONLY process on the port:
& $PY <scratch>\logger.py COM15 420           # forces verbose on, logs 7 min
```

Each forwarded report logs as
`[out] slot N id=.. type=.. len=.. : XX XX XX XX`. For a HalfKay block the first
three bytes are the flash address. A dropped report logs as `[out] DROPPED ...`
with the same detail, which identifies exactly which block was lost.

## Fault injection

Proves the safety mechanisms fire. Two keys, so a stray byte cannot trigger one.

```powershell
& $PY "$BASE\tools\console.py" --keys "!1"    # wedge core 1 -> watchdog resets
& $PY "$BASE\tools\console.py" --keys "!2"    # drop a report -> sticky flag, LED
& $PY "$BASE\tools\console.py" --keys "!3"    # PIO timeout in 6 s -> mid-flash
& $PY "$BASE\tools\console.py" --keys "!4"    # clone failure -> cloneerr
& $PY "$BASE\tools\console.py" --keys "!5"    # drop in 6 s -> aborts a flash
```

`!3` and `!5` are deferred because the flasher holds the console while running,
so there is no way to inject from outside during a flash.

## Verified end-to-end sequence

The full unattended chain, all steps confirmed working together:

1. Ground the contact (by hand or `b`) — key leaves application mode
2. If it lands descriptor-less, the proxy recovers it to the bootloader by
   itself: cold entries, backoff, then an RP2040 reset
3. Flash with `halfkay_flash.py` or the Teensy Loader GUI — 210 blocks, zero
   dropped
4. Reboot — key returns as `1D50:60FC` with all four interfaces
5. `okprobe.py` — firmware answers over raw HID

Real applications have been exercised across all four cloned interfaces in one
session: a slot label configured in the OnlyKey app (raw HID), an encrypted
backup typed into Notepad as 2502 keystrokes (keyboard), and a WebAuthn login at
webauthn.io (FIDO2), ending `dev->PC : 6034 sent, 0 dropped`.
