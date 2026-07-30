# Local changes to Pico-PIO-USB

Vendored from `sekigon-gonnoc/Pico PIO USB` 0.7.2 and patched. It is **not** in
`lib_deps` any more — adding it back installs a pristine copy that shadows this
one and silently reverts everything below.

Every change is marked `// PATCHED` in the source.

## The problem: unbounded hardware waits inside an interrupt

The library waited on PIO hardware flags with spins that had no timeout:

```c
while ((pp->pio_usb_tx->irq & IRQ_TX_ALL_MASK) == 0) { continue; }
while (!(pp->pio_usb_tx->fdebug & stall_mask))      { continue; }
while ((pp->pio_usb_rx->irq & IRQ_RX_ALL_MASK) != 0) { continue; }
```

That is safe only while the state machines behave. When one desyncs — which a
device detaching mid-transfer can cause — the flag never arrives and the loop
never exits.

These run inside the **SOF alarm interrupt**, which is what makes it fatal
rather than merely slow. `sof_timer()` sets `ep->transfer_started = true`, runs
the transaction, then clears it. A hang in the middle means:

- `transfer_started` is never cleared, so
  `pio_usb_host_endpoint_abort_transfer()` spins forever waiting for it — on
  core 1, inside `tuh_task()`
- `cancel_timer_flag` is never cleared, so `pio_usb_host_stop()` spins forever
- the SOF timer never fires again, so the bus stops entirely

Observed symptom: core 1 frozen with `last phase: USBHost.task` while core 0
kept answering the PC perfectly. Because the device stack stayed healthy, every
host→device report was ACKed and then discarded, so a firmware flash reported
all 210 blocks written and `IMG_REBOOT_OK` while the key received nothing. It
looked like a USB protocol problem for a long time.

`pio_usb_host_endpoint_abort_transfer()` is the most likely trigger: TinyUSB
aborts endpoints exactly when a device detaches, which is the moment the bus is
most likely to have desynced.

## The fix: bound every wait, fail the transaction instead of hanging

| File | What changed |
|---|---|
| `pio_usb.c` | `PIO_USB_WAIT_UNTIL()` macro; all TX waits in `send_pre()` and `pio_usb_bus_usb_transfer()` bounded to `PIO_USB_TX_WAIT_US` (2 ms) |
| `pio_usb_ll.h` | `pio_usb_bus_start_receive()` bounded to 2 ms |
| `pio_usb_host.c` | `pio_usb_host_stop()` / `pio_usb_host_restart()` bounded to 50 ms; `abort_transfer()` bounded to 10 ms |

A full-speed packet is at most ~1 ms on the wire and these waits cover a single
packet, so 2 ms is far beyond any legitimate case while remaining a small
fraction of the proxy's 3 s core-1 watchdog. On timeout the transaction is
abandoned; USB retries are normal and recoverable, whereas spinning is not.

`pio_usb_tx_timeouts` counts every give-up. Nonzero means the bus desynced at
least once. `src/main.cpp` reports it in `i` and in the `s` health line, so it
cannot pass unnoticed — a silent recovery would just be a quieter version of the
original bug.

## Evidence it works

A 25-cycle soak (`tools/soak.py`, bootloader ↔ application with a full 210-block
flash each cycle) recorded `piotimeouts=1` on cycle 4 — and that cycle still
passed, as did every cycle after it. The bus genuinely desynced, the bounded
wait gave up, the transaction was abandoned, and the host stack carried on.

Under the original unbounded spin that same event hangs core 1 permanently,
after which every host→device report is ACKed and discarded. A non-zero
`pio_usb_tx_timeouts` is therefore normal and survivable; it is reported rather
than swallowed because a silent recovery is just a quieter version of the bug.

## Relationship to the watchdog

These patches and `PROXY_CORE1_WATCHDOG_MS` are belt and braces, not
alternatives. The patches stop the hang at its source; the watchdog still exists
because this is unlikely to be the only way a bit-banged USB stack can wedge.
