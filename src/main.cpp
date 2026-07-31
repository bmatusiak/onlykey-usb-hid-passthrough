/*
 * USB HID passthrough for the Adafruit Feather RP2040 USB Host.
 *
 *   PC  <--- native USB (device stack, core 0) --- Feather
 *   Feather --- Type-A port (PIO USB host stack, core 1) ---> OnlyKey
 *
 * The four device-side HID interfaces are declared statically from the
 * OnlyKey's own report descriptors, so no descriptor cloning or runtime
 * re-configuration is needed. The device side mirrors the presence of the
 * attached key: we stay detached from the PC until the OnlyKey enumerates, and
 * detach again the moment it goes away. GPIO18 gates 5V to the Type-A port, so
 * a power cycle -- by button or console command -- looks to the PC like the key
 * was physically unplugged and plugged back in.
 *
 * Reports move between the two stacks through per-interface single-producer /
 * single-consumer ring buffers. Per-interface, not shared, so a host that stops
 * draining one interface cannot block traffic on the others -- CTAPHID in
 * particular has timeouts that head-of-line blocking would trip.
 *
 * Cross-core rule: only core 1 may call tuh_*, only core 0 may call tud_* or
 * TinyUSBDevice methods.
 *
 * Requires a 240 MHz CPU clock (see board_build.f_cpu). Pico-PIO-USB accepts
 * 120 MHz too, but the OnlyKey stalls at SET_ADDRESS there and never binds a
 * class driver; 240 MHz gives the timing margin it needs.
 */

#include <Arduino.h>
#include <stdarg.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/watchdog.h>
#include <hardware/sync.h>
#include <pico/bootrom.h>

// pio_usb.h must come first: its presence is what puts tusb_config into
// PIO-host mode when Adafruit_TinyUSB.h is pulled in.
#include "pio_usb.h"

// Counts how often the PATCHED Pico-PIO-USB gave up waiting on a PIO flag.
// Nonzero means the bus desynced and a transaction was abandoned -- recoverable,
// but it is the exact condition that used to hang the host stack forever, so it
// is reported rather than swallowed. See lib/Pico_PIO_USB/PATCHES.md.
extern "C" volatile uint32_t pio_usb_tx_timeouts;

#include "Adafruit_TinyUSB.h"

#include "log_ring.h"

// --------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------

// PROXY_DIAG builds a diagnostic firmware: the USB device configuration is left
// exactly as the core set it up, so the PC keeps its CDC serial port and the log
// is readable over the normal USB cable with no UART adapter. Nothing is
// proxied in this mode -- it only reports what the host port sees.
#ifndef PROXY_DIAG
#define PROXY_DIAG 0
#endif

// PROXY_ENABLE_CDC keeps the USB CDC interface alongside the four HID
// interfaces, giving a COM port on the PC for the command console. The cost is
// that the device is no longer a byte-for-byte OnlyKey clone: it gains a serial
// function, and the HID interfaces shift from MI_00..03 to MI_02..05. Nothing
// binds by interface number -- Windows matches FIDO2 by usage page and the
// OnlyKey app by VID/PID -- so in practice this is invisible.
//
// Set to 0 for a pure HID clone; the console then lives on the UART.
#ifndef PROXY_ENABLE_CDC
#define PROXY_ENABLE_CDC 1
#endif

#if PROXY_DIAG || PROXY_ENABLE_CDC
#define PROXY_LOG Serial // USB CDC
#else
// Without a CDC interface the log lives on the hardware UART:
// TX=GPIO0, RX=GPIO1, 115200 8N1.
#define PROXY_LOG Serial1
#endif

// Identity presented to the PC. Matches the working libcomposite gadget
// (0x16C0/0x0486); the real OnlyKey is 0x1D50/0x60FC. The attached device's
// actual IDs are logged on mount, so switch these if a host filters on them.
#define PROXY_VID 0x16C0
#define PROXY_PID 0x0486
#define PROXY_MANUFACTURER "CRYPTOTRUST"
#define PROXY_PRODUCT "ONLYKEY"
#define PROXY_SERIAL "1000000000"

// How long VBUS stays off during a power cycle. The PC sees this as an unplug,
// so it wants to be long enough to be unambiguous.
#define PROXY_POWER_OFF_MS 750

// Diagnostic mode re-runs status + a power cycle on this period, so a monitor
// attached at any time sees a complete enumeration attempt without interaction.
#define PROXY_DIAG_CYCLE_MS 12000

// A device's HID interfaces mount one at a time. Wait for the arrivals to stop
// before presenting ourselves to the PC, so it enumerates us once, not four
// times.
#define PROXY_SETTLE_MS 500

// GPIO wired to the OnlyKey's bootloader contact, which triggers when pulled to
// ground. Ground is already shared through the Type-A connector, so this needs
// one wire.
//
#define PROXY_BOOTSEL_PIN 6

// How the pin reaches that contact:
//
//  1 = through an N-channel MOSFET / NPN (gate or base on this pin, drain or
//      collector on the contact, source or emitter to ground). Drive HIGH to
//      press. STRONGLY PREFERRED: RP2040 pads come out of reset as inputs with
//      a pull-DOWN, which holds the transistor off, so the contact reads
//      released during reset and while the RP2040 bootloader runs -- a window
//      no firmware can cover. It also keeps the Feather's pin off the
//      OnlyKey's button line entirely, so nothing can back-feed the key while
//      it is unpowered.
//
//  0 = wired straight to the contact. Driven open-drain only: output-low to
//      press, high-Z to release, never driven high. Works, but that same
//      reset-time pull-down sits on the contact and can read as a stuck press
//      -- which is exactly what stopped the key enumerating once wired this
//      way.
// DEFAULT IS 0 (direct wire) because that is what is on the bench. Getting this
// wrong is not a no-op: with a direct wire and ACTIVE_HIGH=1, "released" drives
// the pin LOW, which grounds the key's bootloader contact permanently and stops
// it booting at all. Only set 1 once a transistor is actually fitted.
#ifndef PROXY_BOOTSEL_ACTIVE_HIGH
#define PROXY_BOOTSEL_ACTIVE_HIGH 0
#endif

// How long to hold the contact. A brief tap is all a Teensy-style program
// button needs; the value is generous because holding longer is harmless.
#define PROXY_BOOTSEL_HOLD_MS 300

// Grace period between an interface mounting and us first asking it for a
// report. A device that has only just enumerated may still be initialising --
// notably the OnlyKey coming up in bootloader mode -- and polling it the
// instant it appears is a good way to upset it. Nothing is lost by waiting:
// the device buffers nothing until we ask.
#define PROXY_ARM_DELAY_MS 500

// Ring depth per interface. IN is device->PC (bursty: CTAPHID sends 64-byte
// continuation frames back to back), OUT is PC->device.
#define PROXY_IN_QUEUE_LEN 32
// Each slot is PROXY_MAX_OUT_REPORT bytes to accommodate firmware blocks, so
// depth costs real RAM (4 interfaces x this x 1152 bytes) -- but too shallow
// silently corrupts a firmware write.
//
// Raised from 8 after the Teensy Loader GUI lost 22 blocks in a single flash.
// Eight PIO desyncs during that run each abandoned a transaction, core 1 fell
// behind draining, the queue filled, and reports were dropped -- after the PC
// had already been ACKed. The key enumerated but ran a corrupt image.
#define PROXY_OUT_QUEUE_LEN 16

// How long set_report_thunk() will wait for a free slot before giving up. The
// host has already been told the transfer succeeded by the time we could
// refuse, so waiting is the only way to avoid losing data.
//
// Raised from 250 ms for the same reason. A PIO desync plus recovery can take
// longer than that, and a flashing tool with no health channel -- the Teensy
// GUI, the OnlyKey app, anything that is not tools/halfkay_flash.py -- has no
// way to notice the loss. Our own flasher gates on `piotimeouts` and aborts;
// everything else depends on the proxy simply not dropping.
//
// It must also OUTLAST the chip-erase hold-off below, and that is the binding
// constraint. During the hold-off forward_out() deliberately drains nothing,
// while a GUI that does not wait for the erase keeps sending at ~35 blocks/s.
// At 1500 ms a block was dropped 1.5 s into a 3 s hold-off -- caught in the act
// at address 0x004000. Deepening the queue cannot fix this: 3 s of that rate is
// ~105 blocks, over 120 KB of slots. Blocking the sender is the only thing that
// works, and it is exactly what teensy_loader_cli does with its own sleep.
//
// Safe to raise: the wait already abandons after 20 ms if core 1 has stopped
// ticking, so this only ever stretches for a core 1 that is alive but busy.
#define PROXY_OUT_BLOCK_MS 4000

// Anything this size or smaller is an ordinary HID report and must never block
// the IRQ; larger means a control-pipe firmware block, which may.
#define PROXY_SMALL_REPORT_MAX 64

// How long core 1 may go without completing a loop before we treat it as wedged
// and reset the board.
//
// This exists because a wedge is otherwise INVISIBLE and destructive. Core 1
// runs the host stack, so when it stops, forward_out() stops with it -- but
// core 0 keeps answering the PC perfectly. tud_hid_set_report_cb() is void, so
// a report we cannot queue is dropped after the host has already been told the
// transfer succeeded. The result is a flashing tool that writes all 210 blocks,
// reports success, sends its reboot, logs IMG_REBOOT_OK -- and the key received
// none of it. That is exactly how this failure presented, and it cost a long
// time to find precisely because every layer above claimed everything worked.
//
// Resetting turns that silent corruption into an honest error: our device drops
// off the bus mid-transfer, so the tool fails loudly instead of lying.
//
// Generous enough not to fire on legitimate work: a descriptor dump uses the
// synchronous tuh_* APIs and can hold core 1 for a while, so dumps disarm it.
#define PROXY_CORE1_WATCHDOG_MS 3000

// Pico-PIO-USB waits on PIO hardware flags with no timeout of its own, e.g.
//   while ((pp->pio_usb_tx->irq & IRQ_TX_EOP_MASK) == 0) { continue; }
// so a desynced state machine hangs the host task forever. Nothing inside the
// library recovers from that; the watchdog above is the only way out.

// After a HalfKay block-0 write, hold off before forwarding anything else.
//
// Writing block 0 triggers a full chip erase that takes seconds, during which
// the bootloader NAKs everything. teensy_loader_cli sleeps 3 s here and so does
// tools/halfkay_flash.py -- but a proxy cannot rely on the PC doing that,
// because the PC is talking to US, not to the key. The OnlyKey GUI sends block
// 1 immediately, and the erase window was then absorbed as ~390 retried control
// transfers hammering a busy device (measured: blocks 1/2/3 retried 138/181/73
// times). The flash still completed, but that burst is the most plausible
// trigger for the PIO desync above. Enforcing the delay here makes every
// flashing tool behave, whether or not it waits.
#define PROXY_HALFKAY_ERASE_MS 3000

// The back-pressure wait must outlast the hold-off, or reports that arrive
// while forwarding is deliberately paused are dropped instead of waiting. That
// is not theoretical: it lost a firmware block at 0x004000 during a GUI flash.
static_assert(PROXY_OUT_BLOCK_MS > PROXY_HALFKAY_ERASE_MS,
              "PROXY_OUT_BLOCK_MS must exceed PROXY_HALFKAY_ERASE_MS, or the "
              "erase hold-off itself causes dropped reports");

#define PROXY_MAX_HID 4
static_assert(PROXY_MAX_HID <= CFG_TUD_HID,
              "raise -DCFG_TUD_HID in platformio.ini");

// Logging never touches the port directly: core 1 and the USB IRQ both log, and
// neither may block or race core 0's tud_task(). Everything goes into the ring
// and core 0 drains it in loop(). See include/log_ring.h.
LogRing g_log_ring;

// One message must fit in this buffer. It is a stack buffer and core 1 and the
// USB IRQ both log through here, so it stays small; split a long message into
// several LOGF calls rather than growing it. Truncation is reported, not
// swallowed -- a silently cut log line reads as "that is all there was", which
// is how a truncated `?` was mistaken for a broken help text.
static void log_emit(char const *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int const want = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = '\0';
  g_log_ring.write(buf);
  if (want >= (int)sizeof(buf)) {
    g_log_ring.write("\r\n[log] ** line truncated **\r\n");
  }
}

#define LOGF(...) log_emit(__VA_ARGS__)

// Core 0 only: move buffered log text to the real port, bounded by what the
// port will accept right now so a busy log can never stall loop().
static void drain_log(void) {
  int room = PROXY_LOG.availableForWrite();
  if (room <= 0) {
    return;
  }
  if (room > 128) {
    room = 128;
  }
  while (room-- > 0) {
    int const c = g_log_ring.read();
    if (c < 0) {
      break;
    }
    PROXY_LOG.write((uint8_t)c);
  }
}

// --------------------------------------------------------------------------
// The OnlyKey's report descriptors, as dumped with `usbhid-dump -m 16c0`
// --------------------------------------------------------------------------

// Keyboard: 8-byte input, consumer byte, LED output, 8-byte feature.
static uint8_t const desc_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x05, 0x0C, 0x09, 0xB8, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x7F, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x7F, 0x81, 0x00, 0x09, 0x76, 0x95, 0x08, 0x75, 0x08,
    0xB1, 0x02, 0xC0};

// FIDO2 / U2F (CTAPHID): usage page 0xF1D0, 64 bytes each way.
static uint8_t const desc_fido[] = {
    0x06, 0xD0, 0xF1, 0x09, 0x01, 0xA1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xC0};

// OnlyKey app raw HID: usage page 0xFFAB, 64 bytes each way.
static uint8_t const desc_rawhid[] = {
    0x06, 0xAB, 0xFF, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xC0};

// Teensy seremu debug: usage page 0xFFC9, 64B in / 32B out / 4B feature.
static uint8_t const desc_seremu[] = {
    0x06, 0xC9, 0xFF, 0x09, 0x04, 0xA1, 0x5C, 0x75, 0x08, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x95, 0x40, 0x09, 0x75, 0x81, 0x02, 0x95, 0x20,
    0x09, 0x76, 0x91, 0x02, 0x95, 0x04, 0x09, 0x76, 0xB1, 0x02, 0xC0};

struct proxy_itf_desc_t {
  char const *name;
  uint8_t const *desc;
  uint16_t desc_len;
  uint8_t sig_len; // leading bytes that identify this interface
  uint8_t boot_protocol;
};

// Order defines the interface order the PC sees. Keyboard first, matching the
// OnlyKey's own layout.
static proxy_itf_desc_t const k_itf[PROXY_MAX_HID] = {
    {"keyboard", desc_keyboard, sizeof(desc_keyboard), 4,
     HID_ITF_PROTOCOL_KEYBOARD},
    {"fido2", desc_fido, sizeof(desc_fido), 5, HID_ITF_PROTOCOL_NONE},
    {"rawhid", desc_rawhid, sizeof(desc_rawhid), 5, HID_ITF_PROTOCOL_NONE},
    {"seremu", desc_seremu, sizeof(desc_seremu), 5, HID_ITF_PROTOCOL_NONE},
};

// --------------------------------------------------------------------------
// Device side (core 0)
// --------------------------------------------------------------------------

static Adafruit_USBD_HID g_hid[PROXY_MAX_HID];

// --------------------------------------------------------------------------
// Host side (core 1)
// --------------------------------------------------------------------------

Adafruit_USBH_Host USBHost;

// Maps one of our four interfaces onto the host-side HID instance serving it.
// Written by core 1; core 0 reads `mounted` for status output only.
// Report descriptors are captured from the attached device rather than assumed,
// so anything that enumerates gets proxied -- including the OnlyKey's bootloader,
// whose descriptors are different and not known in advance. The compiled-in
// table is used only to *name* and colour the familiar interfaces.
//
// 256 is not arbitrary: it is CFG_TUH_ENUMERATION_BUFSIZE. TinyUSB does not
// deliver report descriptors larger than that, so nothing bigger can arrive.
#define PROXY_MAX_DESC_LEN 256

struct host_link_t {
  volatile bool mounted;
  uint8_t daddr;
  uint8_t idx;
  bool has_out_ep; // interrupt OUT available, else fall back to control
  int8_t known;    // index into k_itf, or -1 for an unrecognised interface
  uint16_t desc_len;
  uint8_t desc[PROXY_MAX_DESC_LEN];
  uint32_t mounted_at; // millis() when the interface appeared
  bool armed;          // IN endpoint requested yet? see arm_service()
  uint32_t in_count;   // reports received from this interface
  uint32_t rearms;     // times the IN request chain had to be restarted
};
static host_link_t g_link[PROXY_MAX_HID];

// Identity of whatever is currently attached, adopted so that bootloader mode
// (a different VID/PID) is presented to the PC as the bootloader, not as the
// application firmware. Falls back to the compiled-in IDs when nothing is up.
static volatile uint16_t g_dev_vid = 0;
static volatile uint16_t g_dev_pid = 0;

// Presence mirroring. Core 1 decides whether a key is attached; core 0 owns the
// device stack and is the only core allowed to call detach()/attach().
static volatile bool g_pc_present_req; // core 1 writes, core 0 reads
static volatile bool g_pc_attached;    // core 0 only
static uint32_t g_link_settle_at;      // core 1 only; 0 = nothing pending

// --------------------------------------------------------------------------
// Cross-core report queues
// --------------------------------------------------------------------------

// Lock-free for one producer and one consumer. The producer owns `head`, the
// consumer owns `tail`; the barrier in commit() ensures a slot's contents are
// visible on the other core before the index that publishes it moves.
template <typename T, uint16_t N> struct spsc_queue {
  T slot[N];
  volatile uint16_t head;
  volatile uint16_t tail;

  // Producer side.
  T *reserve(void) {
    if ((uint16_t)((head + 1) % N) == tail) {
      return nullptr; // full
    }
    return &slot[head];
  }
  void commit(void) {
    __dmb();
    head = (uint16_t)((head + 1) % N);
  }

  // Consumer side.
  T *peek(void) {
    if (tail == head) {
      return nullptr; // empty
    }
    __dmb();
    return &slot[tail];
  }
  void pop(void) { tail = (uint16_t)((tail + 1) % N); }
  void flush(void) { tail = head; }
};

struct in_report_t {
  uint8_t len;
  uint8_t data[CFG_TUH_HID_EPIN_BUFSIZE];
};

// Big enough for the OnlyKey's HalfKay bootloader, which writes firmware in
// 1089-byte SET_REPORTs (1 report-ID byte + a 64-byte header + a 1024-byte
// block). Ordinary HID reports are 64 bytes; this is sized for the worst case
// because a truncated firmware block would be far worse than the wasted RAM.
// Note len must be 16-bit -- 1089 does not fit in a uint8_t.
#define PROXY_MAX_OUT_REPORT 1152

struct out_report_t {
  uint8_t report_id;
  uint8_t report_type;
  uint16_t len;
  uint8_t data[PROXY_MAX_OUT_REPORT];
};

// device -> PC: produced by core 1, consumed by core 0.
static spsc_queue<in_report_t, PROXY_IN_QUEUE_LEN> g_inq[PROXY_MAX_HID];
// PC -> device: produced by core 0, consumed by core 1.
static spsc_queue<out_report_t, PROXY_OUT_QUEUE_LEN> g_outq[PROXY_MAX_HID];

// Each counter is written by exactly one core.
static volatile uint32_t g_in_sent, g_in_dropped;
static volatile uint32_t g_out_sent, g_out_dropped;
// Set the first time a PC->device report is discarded, and never cleared while
// the board runs. A drop is data the PC was told we delivered, so it must not
// scroll away in the log -- the LED stays lit and status keeps saying so until
// the board is reset.
static volatile bool g_out_drop_flag;

// Interfaces the device offered that we could not clone: descriptor missing,
// too large, or no free slot. Cleared when the device detaches, so it always
// describes the current attachment.
//
// Worth counting rather than only logging. A partial clone is a quiet
// degradation, not a crash: the PC simply sees fewer interfaces than the key
// has, and since host software locates raw HID by interface NUMBER, a missing
// interface silently shifts the rest and sends an app talking to the wrong one.
// That is indistinguishable from a protocol bug from the outside.
static volatile uint8_t g_clone_failures;

// --------------------------------------------------------------------------
// VBUS control -- owned by core 1, requested by core 0
// --------------------------------------------------------------------------

#ifndef PIN_5V_EN_STATE
#define PIN_5V_EN_STATE 1
#endif

enum power_cmd_t : uint8_t {
  PWR_CMD_NONE = 0,
  PWR_CMD_CYCLE,
  PWR_CMD_OFF,
  PWR_CMD_ON,
  // Power-cycle with the bootloader contact held down throughout, so the key
  // samples it while booting. See bootsel_cold_entry() for why this is the only
  // recovery that works on a key with no application firmware.
  PWR_CMD_BOOTSEL_CYCLE,
};

// Has any device enumerated since VBUS last came up? Gates auto-recovery: a key
// that has appeared once is rebooting rather than stuck, and must be left alone.
// Cleared on every power-off so a fresh power-up can recover again.
static bool g_seen_device = false;

static volatile uint8_t g_power_cmd; // core 0 writes, core 1 consumes
static volatile bool g_power_is_on;  // core 1 writes, core 0 reads
static uint32_t g_power_off_until;   // core 1 only

// Core 1 bumps this every pass through loop1(). Core 0 watches it to tell
// "host stack running, no device found" apart from "core 1 wedged", which
// otherwise look identical from the outside.
static volatile uint32_t g_core1_ticks;

// Which step of loop1() core 1 entered last. When it wedges, this says where --
// otherwise a stall is just a frozen counter with no indication of the cause.
enum core1_phase_t : uint8_t {
  C1_IDLE = 0,
  C1_HOST_TASK,
  C1_POWER,
  C1_LINK,
  C1_DUMP,
  C1_POLL,
  C1_FORWARD_OUT,
  C1_MOUNT_CB,
  C1_UMOUNT_CB,
  C1_REPORT_CB,
};
static volatile uint8_t g_core1_phase = C1_IDLE;
static char const *const kPhaseName[] = {
    "idle",      "USBHost.task", "power_service", "link_service",
    "dump",      "poll_count",   "forward_out",   "mount_cb",
    "umount_cb", "report_cb",
};
// Set by core 1 if it cannot start the host stack at all.
static volatile bool g_core1_fatal;

// core 1 can be launched before core 0 reaches setup(), so it waits for this
// before logging -- otherwise the clock-check failure message, the one message
// that matters most, is written to an uninitialised port and lost.
static volatile bool g_log_ready;

// Published by core 1 so core 0 can report host-stack state without calling
// tuh_* itself. `g_host_started` distinguishes "no device found" from
// "USBHost.begin() never returned"; the counts show what the HID driver bound.
static volatile bool g_host_started;
static volatile uint8_t g_host_hid_count;

// Address of the enumerated device, 0 if none. Set on device-level attach, so
// it is populated even when no class driver binds.
static volatile uint8_t g_dev_addr;
// Core 0 sets this to ask core 1 for a full descriptor dump. The sync
// descriptor APIs must run in loop1 context, never inside a TinyUSB callback.
static volatile bool g_dump_req;

// The BOOT button is a plain GPIO on this board ("Boot0 also on GPIO #7" in the
// variant), so it reads normally at runtime -- no need for the flash-CS BOOTSEL
// trick other RP2040 boards require. It only means "bootloader" when held down
// during reset. Core 0 owns these.
static uint32_t g_btn_presses = 0;
static uint32_t g_btn_last_ms = 0;

// Result of the one-shot D+/D- probe core 1 runs before PIO claims the pins.
// This is the only trustworthy read of which line carries D+.
static char const *volatile g_pad_probe_text;

// Core 1: publish whether a key is attached. Dropping presence also discards
// queued PC->device reports, so nothing stale is delivered after a reconnect.
static void pc_present_set(bool present) {
  if (!present) {
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      g_outq[i].flush();
    }
  }
  g_pc_present_req = present;
}

// UNPROVEN. Observation: cutting VBUS drops the key off the bus but does not
// make it re-run its startup -- entering the bootloader by holding the contact
// across a VBUS cycle never worked. One hypothesis was leakage through the data
// lines keeping the device alive; that was never confirmed and should not be
// stated as fact.
//
// This releases the pads during power-off to remove any such path, which is also
// what a genuinely disconnected bus looks like (SE0). It is OFF by
// default: releasing the pads mid-flight wedges Pico-PIO-USB, so the host stack
// stops servicing and the command that would restore power never runs, leaving
// the board reachable only via a manual reset. Kept rather than deleted because
// the underlying effect is real -- doing it safely needs the PIO state machines
// stopped first, which is unfinished work.
#ifndef PROXY_CUT_DATA_LINES
#define PROXY_CUT_DATA_LINES 0
#endif

#if PROXY_CUT_DATA_LINES
static void host_pins_release(void) {
  gpio_set_function(PIN_USB_HOST_DP, GPIO_FUNC_SIO);
  gpio_set_function(PIN_USB_HOST_DP + 1, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_USB_HOST_DP, GPIO_OUT);
  gpio_set_dir(PIN_USB_HOST_DP + 1, GPIO_OUT);
  gpio_put(PIN_USB_HOST_DP, 0);
  gpio_put(PIN_USB_HOST_DP + 1, 0);
}

// Hand the pads back to Pico-PIO-USB, which owns PIO0.
static void host_pins_restore(void) {
  gpio_set_function(PIN_USB_HOST_DP, GPIO_FUNC_PIO0);
  gpio_set_function(PIN_USB_HOST_DP + 1, GPIO_FUNC_PIO0);
}
#endif // PROXY_CUT_DATA_LINES

static void power_apply(bool on) {
  if (on) {
    // Restore the pads BEFORE raising VBUS, never after: if anything below
    // wedges, the rail is already back and the board stays reachable.
#if PROXY_CUT_DATA_LINES
    host_pins_restore();
#endif
  }
  if (!on) {
    // Don't wait for the umount callbacks -- drop the PC side immediately so
    // the unplug it sees lines up with VBUS actually going away.
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      g_link[i].mounted = false;
    }
    g_link_settle_at = 0;
    pc_present_set(false);
    g_seen_device = false; // fresh power-up may legitimately need recovery
#if PROXY_CUT_DATA_LINES
    host_pins_release(); // before VBUS drops -- see the note above
#endif
  }

  digitalWrite(PIN_5V_EN, on ? PIN_5V_EN_STATE : !PIN_5V_EN_STATE);
  g_power_is_on = on;
}

// Non-blocking: the host stack must keep running through the off window so it
// notices the disconnect and re-enumerates on the way back up.
// How long to let the key boot after VBUS returns, before pressing the contact.
// Pressing while it is still coming up is the same as not pressing at all.
#define PROXY_BOOTSEL_COLD_BOOT_MS 1500

// How long the contact is held down for the press itself. The bootloader
// triggers on RELEASE, so this only has to be long enough to register as a
// press -- it is not a "hold until something happens" period.
#define PROXY_BOOTSEL_COLD_HOLD_MS 300

// How long VBUS stays off during a cold entry.
//
// Much longer than PROXY_POWER_OFF_MS (750 ms), which is tuned for "make the PC
// see an unplug" rather than "actually power the key down". A 750 ms cut did
// NOT reset the key: repeated cold entries at that duration left it stuck
// descriptor-less, while an RP2040 reset -- which keeps VBUS off far longer,
// across the whole reset and boot -- recovered it every time. The key evidently
// rides through a short cut on its own decoupling.
#define PROXY_BOOTSEL_COLD_OFF_MS 3000

// Defined further down with the rest of the contact handling; needed here.
static void bootsel_assert(void);
static void bootsel_release(void);

// Set by core 1 around an operation that legitimately blocks it for longer than
// PROXY_CORE1_WATCHDOG_MS, so the watchdog does not mistake deliberate work for
// a wedge and reset the board out from under it.
static volatile bool g_core1_long_op = false;

// Core 1: the only recovery that works on a key with no application firmware.
//
// A Teensy samples its program contact WHILE BOOTING. Entering the bootloader
// invalidates the application firmware, so afterwards there is no running
// firmware left to notice a button press -- which means pulsing the contact on
// a powered-up key can never do anything. Measured: three auto-pulses, three
// manual pulses and a plain VBUS cycle all failed to recover a descriptor-less
// key, and a single RP2040 reset recovered it instantly. The reset works only
// because it happens to drop VBUS while GPIO 6 reverts to input-with-pull-down,
// holding the contact grounded across the key's power-up.
//
// So do that deliberately instead of relying on a side effect: hold the
// contact, cut VBUS, restore it, and keep holding well past the point where the
// key has sampled the pin.
//
// This blocks core 1 for several seconds, which is far longer than
// PROXY_CORE1_WATCHDOG_MS. Without telling the watchdog, it fires mid-sequence
// and resets the board -- and since a reset restores the defaults that started
// the recovery in the first place, the board reset-loops forever. That happened:
// a 3 s power-off plus a 4 s hold against a 3 s watchdog, cycling endlessly and
// taking the console down every time. g_core1_long_op is what stops it.
static void bootsel_cold_entry(void) {
  LOGF("[bsel] cold bootloader entry: contact pressed across a power cycle\r\n");
  g_core1_long_op = true;

  // Power-cycle first, with the contact RELEASED, and let the key boot.
  power_apply(false);
  delay(PROXY_BOOTSEL_COLD_OFF_MS);
  power_apply(true);
  delay(PROXY_BOOTSEL_COLD_BOOT_MS); // let it come up before pressing anything

  // Then press and release, which is what a human does with the button. The
  // bootloader triggers on RELEASE, not while held -- holding the contact down
  // prevents the very thing this is trying to achieve, which is why an earlier
  // version that held it through enumeration never worked.
  bootsel_assert();
  delay(PROXY_BOOTSEL_COLD_HOLD_MS);
  bootsel_release();

  g_core1_long_op = false;
  LOGF("[bsel] contact released, key should now enter its bootloader\r\n");
}

static void power_service(void) {
  uint8_t const cmd = g_power_cmd;
  if (cmd != PWR_CMD_NONE) {
    g_power_cmd = PWR_CMD_NONE;
    switch (cmd) {
    case PWR_CMD_CYCLE:
      LOGF("[pwr] cycling VBUS for %u ms\r\n", (unsigned)PROXY_POWER_OFF_MS);
      power_apply(false);
      g_power_off_until = millis() + PROXY_POWER_OFF_MS;
      break;
    case PWR_CMD_OFF:
      power_apply(false);
      g_power_off_until = 0; // stays off until something turns it back on
      // Turning VBUS off detaches the whole USB device, console included -- a
      // USB device cannot drop only some of its interfaces. That is safe
      // because the BOOT button toggles power in hardware and always works,
      // so there is no way to strand the board with no route back.
      LOGF("[pwr] VBUS off (console drops with it; BOOT button toggles it "
           "back)\r\n");
      break;
    case PWR_CMD_ON:
      LOGF("[pwr] VBUS on\r\n");
      g_power_off_until = 0;
      power_apply(true);
      break;
    case PWR_CMD_BOOTSEL_CYCLE:
      bootsel_cold_entry();
      break;
    default:
      break;
    }
  }

  if (g_power_off_until && (int32_t)(millis() - g_power_off_until) >= 0) {
    g_power_off_until = 0;
    LOGF("[pwr] VBUS back on\r\n");
    power_apply(true);
  }
}

// --------------------------------------------------------------------------
// Interface matching
// --------------------------------------------------------------------------

// Identify which of our four interfaces a freshly mounted host interface is,
// by exact descriptor match first, then by the leading usage-page bytes.
static int8_t match_interface(uint8_t const *desc, uint16_t len) {
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (len == k_itf[i].desc_len && memcmp(desc, k_itf[i].desc, len) == 0) {
      return (int8_t)i;
    }
  }
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (len >= k_itf[i].sig_len &&
        memcmp(desc, k_itf[i].desc, k_itf[i].sig_len) == 0) {
      LOGF("[map] inexact match on %s (got %u bytes, expected %u)\r\n",
           k_itf[i].name, len, k_itf[i].desc_len);
      return (int8_t)i;
    }
  }
  return -1;
}

// Print an unknown descriptor in the \xNN form used by the gadget script, so it
// can be pasted straight into the table above.
static void dump_descriptor(uint8_t const *desc, uint16_t len) {
  LOGF("\"");
  for (uint16_t i = 0; i < len; i++) {
    LOGF("\\x%02X", desc[i]);
  }
  LOGF("\"\r\n");
}

// --------------------------------------------------------------------------
// Core 0 -- device stack, LED, console
// --------------------------------------------------------------------------

// One thunk per interface: the Adafruit callback signature carries the report
// ID but not the interface, so the interface index is baked in here.
template <uint8_t ITF>
static void set_report_thunk(uint8_t report_id, hid_report_type_t report_type,
                             uint8_t const *buffer, uint16_t bufsize) {
  // Interrupt-OUT traffic arrives with report_id == 0 and the raw report in the
  // buffer. None of the OnlyKey's descriptors declare report IDs, so the buffer
  // passes through untouched.
  // Returning without queueing still ACKs the report to the host, so a drop is
  // silent data loss the host believes succeeded -- that is how 162 of 210
  // firmware blocks were lost while the flasher reported success.
  //
  // But this callback runs in USB IRQ context, and tud_task() is what re-arms
  // the interrupt OUT endpoint after each packet. Blocking here therefore
  // prevents the very draining it is waiting for: the first few packets land,
  // the queue backs up, and interrupt OUT wedges permanently.
  //
  // So only the large control-pipe firmware blocks wait. Those come from a
  // synchronous host that sends one at a time with core 1 otherwise idle, so a
  // brief spin is safe and prevents corruption. Ordinary reports (<= 64 bytes,
  // every real HID interface here) never block; the queue is deep enough that
  // core 1 keeps up, and a dropped keystroke is survivable where a dropped
  // firmware block is not.
  //
  // The spin is pointless once core 1 has stopped draining, so watch its tick
  // counter and give up the moment it stops moving. Waiting 250 ms per report
  // for a core that is never coming back just slows the inevitable, and the
  // watchdog needs core 0 running to notice and reset.
  out_report_t *slot = g_outq[ITF].reserve();
  if (!slot && bufsize > PROXY_SMALL_REPORT_MAX) {
    uint32_t const started = millis();
    uint32_t const ticks_at_entry = g_core1_ticks;
    while (!slot && (millis() - started) < PROXY_OUT_BLOCK_MS) {
      // 20 ms without core 1 completing a single loop means it is wedged, not
      // merely busy: it normally ticks millions of times a minute.
      if ((millis() - started) > 20 && g_core1_ticks == ticks_at_entry) {
        break;
      }
      slot = g_outq[ITF].reserve();
    }
  }
  if (!slot) {
    // Silent loss. We have already ACKed this transfer, so the PC believes it
    // succeeded and there is no way left to tell it otherwise -- record it
    // loudly instead, because a run that ends "210 blocks sent, success" while
    // the device received nothing is the single most misleading state this
    // firmware can be in.
    g_out_dropped++;
    g_out_drop_flag = true; // sticky: surfaces in status and on the LED

    // Say WHAT was lost, not just that something was. "drops=1" leaves it
    // unknowable whether a firmware block went missing (corrupt image) or an
    // incidental report did (harmless) -- and those need completely different
    // responses. For a HalfKay block the first three bytes are the flash
    // address, so this identifies exactly which block to distrust.
    LOGF("[out] DROPPED itf %u id=%u type=%u len=%u : %02X %02X %02X %02X\r\n",
         ITF, report_id, (unsigned)report_type, bufsize,
         bufsize > 0 ? buffer[0] : 0, bufsize > 1 ? buffer[1] : 0,
         bufsize > 2 ? buffer[2] : 0, bufsize > 3 ? buffer[3] : 0);
    return;
  }
  if (bufsize > sizeof(slot->data)) {
    bufsize = sizeof(slot->data);
  }
  slot->report_id = report_id;
  slot->report_type = (uint8_t)report_type;
  slot->len = bufsize; // 16-bit: firmware blocks are far larger than 255 bytes
  memcpy(slot->data, buffer, bufsize);
  g_outq[ITF].commit();
}

static_assert(PROXY_MAX_HID == 4,
              "keep the set_report_thunk<> entries below in sync");
static Adafruit_USBD_HID::set_report_callback_t const k_set_cb[PROXY_MAX_HID] = {
    set_report_thunk<0>,
    set_report_thunk<1>,
    set_report_thunk<2>,
    set_report_thunk<3>,
};

static bool core1_alive(void); // defined with the LED code below

static void print_status(void) {
  LOGF("---- proxy status ----\r\n");
  uint8_t const ph = g_core1_phase;
  LOGF("core1     : %s, %lu ticks, last phase: %s\r\n",
       g_core1_fatal ? "FATAL" : (core1_alive() ? "running" : "STALLED"),
       (unsigned long)g_core1_ticks,
       ph < (sizeof(kPhaseName) / sizeof(kPhaseName[0])) ? kPhaseName[ph]
                                                        : "?");
  LOGF("host stack: %s, %u HID itf bound\r\n",
       g_host_started ? "started" : "NOT STARTED", g_host_hid_count);
  if (pio_usb_tx_timeouts) {
    LOGF("PIO bus   : %lu timeout(s) -- the bus desynced and a transaction was "
         "abandoned (this used to hang the stack forever)\r\n",
         (unsigned long)pio_usb_tx_timeouts);
  }
  LOGF("VBUS      : %s (GPIO%u reads %u)\r\n", g_power_is_on ? "on" : "off",
       (unsigned)PIN_5V_EN, gpio_get(PIN_5V_EN) ? 1u : 0u);

  // Raw pad levels, for reference only: PIO actively drives these lines, so a
  // sample taken at an arbitrary moment says nothing reliable about what is
  // attached. The trustworthy measurement is the pre-init probe below.
  LOGF("raw pads  : GPIO%u=%u GPIO%u=%u (PIO is driving these; not a presence "
       "test)\r\n",
       (unsigned)PIN_USB_HOST_DP, gpio_get(PIN_USB_HOST_DP) ? 1u : 0u,
       (unsigned)PIN_USB_HOST_DP + 1,
       gpio_get(PIN_USB_HOST_DP + 1) ? 1u : 0u);
  LOGF("pad probe : %s\r\n", g_pad_probe_text ? g_pad_probe_text : "(not run)");
  LOGF("PC side   : %s\r\n", g_pc_attached
                                 ? (TinyUSBDevice.mounted() ? "mounted"
                                                            : "attached")
                                 : "detached");
  LOGF("attached  : %04X:%04X\r\n", g_dev_vid, g_dev_pid);
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    host_link_t const *l = &g_link[i];
    if (!l->mounted) {
      LOGF("slot %u    : -\r\n", i);
      continue;
    }
    LOGF("slot %u    : %s, %u-byte desc, OUT ep %s, %lu in, %lu rearm\r\n", i,
         l->known >= 0 ? k_itf[l->known].name : "UNKNOWN (bootloader?)",
         l->desc_len, l->has_out_ep ? "yes" : "no",
         (unsigned long)l->in_count, (unsigned long)l->rearms);
  }
  LOGF("dev->PC   : %lu sent, %lu dropped\r\n", (unsigned long)g_in_sent,
       (unsigned long)g_in_dropped);
  LOGF("PC->dev   : %lu sent, %lu dropped\r\n", (unsigned long)g_out_sent,
       (unsigned long)g_out_dropped);
  if (g_out_drop_flag) {
    LOGF("  ** reports were DROPPED after being ACKed to the PC -- whatever "
         "sent them was told they succeeded **\r\n");
  }
  LOGF("BOOT btn  : GPIO%u reads %s, %lu press(es)",
       (unsigned)PIN_BUTTON, digitalRead(PIN_BUTTON) == LOW ? "DOWN" : "up",
       (unsigned long)g_btn_presses);
  if (g_btn_presses) {
    LOGF(", last %lu ms ago", (unsigned long)(millis() - g_btn_last_ms));
  }
  LOGF("\r\n");
  LOGF("keys      : p=power-cycle 0=off 1=on i=info\r\n");
}

// One line, fixed field order, easy to parse. This is the interface a script
// gates on -- the human-readable block above is for people and its layout is
// not stable enough to depend on.
//
// The whole point is fail-fast: a flashing tool reads this between batches and
// aborts the moment anything is wrong, instead of writing 210 blocks into a
// dead proxy and reporting success. Every field here is something that, if
// wrong, means the bytes did not arrive.
//
//   core1=ok|STALLED|FATAL   is the host stack running at all
//   drops=N dropflag=0|1     data discarded after the PC was told it landed
//   sent=N                   host->device reports actually delivered
//   itf=N                    interfaces currently linked
//   vid/pid                  what is attached, so a script can confirm the
//                            key is in the mode it thinks it is
static void print_health(void) {
  LOGF("[health] core1=%s drops=%lu dropflag=%u sent=%lu in=%lu indrops=%lu "
       "piotimeouts=%lu cloneerr=%u itf=%u mounted=%u vid=%04X pid=%04X\r\n",
       g_core1_fatal ? "FATAL" : (core1_alive() ? "ok" : "STALLED"),
       (unsigned long)g_out_dropped, g_out_drop_flag ? 1u : 0u,
       (unsigned long)g_out_sent, (unsigned long)g_in_sent,
       (unsigned long)g_in_dropped, (unsigned long)pio_usb_tx_timeouts,
       g_clone_failures, g_host_hid_count,
       TinyUSBDevice.mounted() ? 1u : 0u, g_dev_vid, g_dev_pid);
}

// Zero the counters so a run's numbers describe only that run.
//
// This is the one way to clear the sticky drop flag without a reset. That is
// deliberate: the flag exists so a drop cannot scroll away unnoticed, and an
// operator explicitly starting a fresh run is a different act from the
// firmware quietly forgetting.
static void reset_counters(void) {
  g_out_sent = 0;
  g_out_dropped = 0;
  g_in_sent = 0;
  g_in_dropped = 0;
  g_out_drop_flag = false;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    g_link[i].in_count = 0;
    g_link[i].rearms = 0;
  }
  LOGF("[health] counters zeroed\r\n");
}

// --------------------------------------------------------------------------
// Fault injection
// --------------------------------------------------------------------------
//
// Every safety mechanism here exists for a failure that is rare, hard to
// provoke, and catastrophic when missed. That combination means they are
// almost never exercised -- the 25-cycle soak passed without the watchdog
// firing once, so "the tests pass" said nothing at all about whether it works.
// An untested recovery path is not a recovery path.
//
// So each one can be triggered on demand. Two keys are required (`!` then a
// digit) so a stray byte on the console cannot fire one: the console is the
// rig's control channel and single characters there already power-cycle the
// port and reboot the board.
//
//   !1  wedge core 1            -> the watchdog must reset the board
//   !2  discard a report now    -> sticky drop flag, LED, status warning
//   !3  PIO timeout in 6 s      -> lands mid-flash; flasher must refuse the image
//   !4  failed interface clone  -> cloneerr, LED
//   !5  discard a report in 6 s -> lands mid-flash; flasher must abort
//
// !3 and !5 are deferred because the flasher holds the console for its own
// health checks while it runs, so there is no way to inject during a flash from
// outside. Arming first and letting the fault land mid-transfer is the only way
// to prove those gates.
//
// Note the difference between !2 and !5. A drop flag left over from an EARLIER
// run must not block a new flash -- the flasher zeroes the counters at start,
// which is the documented way to begin a clean run. What must abort a flash is
// a drop that happens DURING it. Only !5 tests that.
static volatile bool g_fault_arm;         // next console byte selects a fault
static volatile bool g_fault_wedge;       // core 1 spins forever when set
static volatile uint32_t g_fault_pio_at;  // deferred PIO-timeout injection
static volatile uint32_t g_fault_drop_at; // deferred dropped-report injection

#define PROXY_FAULT_DELAY_MS 6000

static void fault_service(void) {
  if (g_fault_pio_at && (int32_t)(millis() - g_fault_pio_at) >= 0) {
    g_fault_pio_at = 0;
    pio_usb_tx_timeouts++;
    LOGF("[fault] injected a PIO timeout (now %lu)\r\n",
         (unsigned long)pio_usb_tx_timeouts);
  }
  if (g_fault_drop_at && (int32_t)(millis() - g_fault_drop_at) >= 0) {
    g_fault_drop_at = 0;
    g_out_dropped++;
    g_out_drop_flag = true;
    LOGF("[fault] injected a dropped report (drops=%lu)\r\n",
         (unsigned long)g_out_dropped);
  }
}

static void fault_select(char which) {
  g_fault_arm = false;
  switch (which) {
  case '1':
    LOGF("[fault] wedging core 1 -- the watchdog should reset us in %u ms\r\n",
         (unsigned)PROXY_CORE1_WATCHDOG_MS);
    g_fault_wedge = true;
    break;
  case '2':
    g_out_dropped++;
    g_out_drop_flag = true;
    LOGF("[fault] discarded a report (drops=%lu)\r\n",
         (unsigned long)g_out_dropped);
    break;
  case '3':
    g_fault_pio_at = millis() + PROXY_FAULT_DELAY_MS;
    LOGF("[fault] PIO timeout armed for %u ms from now\r\n",
         (unsigned)PROXY_FAULT_DELAY_MS);
    break;
  case '4':
    g_clone_failures++;
    LOGF("[fault] failed an interface clone (cloneerr=%u)\r\n",
         g_clone_failures);
    break;
  case '5':
    g_fault_drop_at = millis() + PROXY_FAULT_DELAY_MS;
    LOGF("[fault] dropped report armed for %u ms from now\r\n",
         (unsigned)PROXY_FAULT_DELAY_MS);
    break;
  default:
    LOGF("[fault] unknown fault '%c' -- 1=wedge 2=drop 3=pio 4=clone "
         "5=drop-in-6s\r\n",
         which);
    break;
  }
}

// When set, status repeats on its own every 3 s. Off by default so the log
// stays quiet and command output is easy to read.
static bool g_heartbeat = false;

// Log every host->device report as it is forwarded. Off by default: a firmware
// flash produces one line per block.
static volatile bool g_out_verbose = false;

// --------------------------------------------------------------------------
// OnlyKey bootloader contact (core 0 only)
// --------------------------------------------------------------------------

static uint32_t g_bootsel_until = 0; // 0 = released
// Held indefinitely by the 'G' command until 'g'. Kept separate from the timed
// hold above so the automatic recovery cannot release a deliberate manual hold
// out from under whoever is testing with it.
static volatile bool g_bootsel_held = false;


// Also the power-on state, so a reset never leaves the contact held.
static void bootsel_release(void) {
#if PROXY_BOOTSEL_ACTIVE_HIGH
  // Drive low: transistor off, contact floats. Driven rather than left as an
  // input so the line cannot be nudged by noise on a long lead.
  pinMode(PROXY_BOOTSEL_PIN, OUTPUT);
  digitalWrite(PROXY_BOOTSEL_PIN, LOW);
#else
  pinMode(PROXY_BOOTSEL_PIN, INPUT); // high-Z; never drive this pin high
#endif
}

static void bootsel_assert(void) {
  pinMode(PROXY_BOOTSEL_PIN, OUTPUT);
#if PROXY_BOOTSEL_ACTIVE_HIGH
  digitalWrite(PROXY_BOOTSEL_PIN, HIGH); // turn the transistor on
#else
  digitalWrite(PROXY_BOOTSEL_PIN, LOW); // pull the contact to ground directly
#endif
}

// If a key boots with no usable application firmware it may not enumerate at
// all -- no descriptor, nothing on the bus, indistinguishable from an empty
// port. Poking the bootloader contact can still drop it into the bootloader,
// which does enumerate, and from there it can be reflashed. So when the port
// stays silent for a while, try the contact before giving up.
#ifndef PROXY_BOOTSEL_AUTO
#define PROXY_BOOTSEL_AUTO 1
#endif
#define PROXY_BOOTSEL_AUTO_MS 6000 // silence this long before trying
#define PROXY_BOOTSEL_AUTO_MAX 3   // then stop, rather than poking forever

// Much longer wait once a device has been seen: it is probably rebooting into
// newly flashed firmware, and a reboot can take well over PROXY_BOOTSEL_AUTO_MS.
#define PROXY_BOOTSEL_REBOOT_GRACE_MS 30000

// Longest gap between automatic recovery attempts once they start backing off.
// Recovery never gives up -- an unattended rig that strands itself is useless --
// but an empty port must not be power-cycled every few seconds forever, because
// each cycle re-enumerates the whole device and takes the console with it.
#define PROXY_BOOTSEL_BACKOFF_MAX_MS 120000

// Cold entries to try before escalating to a full RP2040 reset. Kept low
// because the reset is the recovery measured to work every time on a
// descriptor-less key, while a cold entry sometimes does not.
//
// WHY is not established. This escalation is empirical: a reset works every
// time, and that is the entire justification.
//
// Three candidate mechanisms have been ruled out by measurement, so do not
// spend time re-deriving them:
//
//   - VBUS cycling genuinely powers the key down. With VBUS off it unmounts and
//     sends nothing for 15 s; on `p` it runs its LED boot sequence.
//   - The data lines do not sustain the key. The proxy keeps running during a
//     VBUS-off, so D+/D- ARE still driven, and the key dies anyway.
//   - GPIO 6's reset pull-down does not enter the bootloader. In RP2040 BOOTSEL
//     every pad sits at reset state through the key's whole power-up, and the
//     key returns in APPLICATION mode.
#define PROXY_BOOTSEL_RESET_AFTER 2

// Resets to try before giving up and waiting for a human.
//
// Counted in a watchdog scratch register, NOT in RAM: a reset clears RAM, so a
// RAM counter can never notice that resetting is not helping, and the board
// loops forever power-cycling the key. Giving up eventually is the lesser
// evil -- an endless reset loop is not "unattended", it is a board thrashing
// with the console dropping out every few seconds.
#define PROXY_BOOTSEL_MAX_RESETS 3

// Automatic recovery can be switched off ('A').
//
// It power-cycles the key every few seconds while the port is silent, which is
// right for unattended running but makes the board impossible to experiment
// with by hand: the console drops with every cycle, and the key never gets to
// sit powered long enough to poke at. Anything characterising the contact
// behaviour wants this off.
static volatile bool g_bootsel_auto = true;

static void bootsel_service(void) {
  if (g_bootsel_held) {
    return; // manual hold ('G'); nothing automatic may touch the contact
  }
  if (g_bootsel_until && (int32_t)(millis() - g_bootsel_until) >= 0) {
    g_bootsel_until = 0;
    bootsel_release();
    LOGF("[bsel] contact released\r\n");
  }

#if PROXY_BOOTSEL_AUTO
  static uint32_t idle_since = 0;
  static uint8_t attempts = 0;

  bool linked = false;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    linked = linked || g_link[i].mounted;
  }

  if (!g_bootsel_auto) {
    return; // switched off with 'A' so the board can be poked at by hand
  }

  // A live device means there is nothing to recover from.
  if (linked) {
    idle_since = millis();
    attempts = 0;
    // Recovery worked (or was never needed): forget the escalation history,
    // otherwise the count survives forever and the next genuine problem gets
    // fewer attempts than it should.
    watchdog_hw->scratch[4] = 0;
    g_seen_device = true;
    return;
  }

  // No power: nothing can enumerate, so do not count the silence against the
  // key. But do NOT clear `attempts` here.
  //
  // That was a self-defeating bug: a cold entry turns VBUS off as part of its
  // own sequence, so this branch fired every time and reset the counter. The
  // count never got past 1, the escalation to a reset could never trigger, and
  // the board cold-entried forever -- logging "try 1" each time, which was the
  // visible tell. A recovery must not erase the evidence of its own failures.
  if (!g_power_is_on) {
    idle_since = millis();
    return;
  }

  // A key that appeared and then went away is probably rebooting -- most likely
  // into firmware just flashed through us -- and poking the contact then knocks
  // it straight back into the bootloader, looping forever. So wait much longer
  // in that case rather than refusing outright: a hard latch never cleared on a
  // physical unplug, leaving a genuinely stuck key unrecoverable.
  uint32_t quiet_for =
      g_seen_device ? PROXY_BOOTSEL_REBOOT_GRACE_MS : PROXY_BOOTSEL_AUTO_MS;

  // Back off as attempts pile up.
  //
  // Retrying forever at a fixed interval is right for a key that might still
  // come back, but with nothing attached at all it power-cycles every few
  // seconds indefinitely -- and since cutting VBUS re-enumerates the whole
  // device, that takes the console down with it every time and makes the board
  // almost impossible to talk to. Observed while debugging exactly that.
  //
  // So keep trying (never giving up is what makes the rig unattended) but slow
  // down, up to PROXY_BOOTSEL_BACKOFF_MAX_MS between attempts. A key that is
  // genuinely coming back is recovered by the first few tries; anything beyond
  // that is a port with nothing in it, and there is no hurry.
  if (attempts > 2) {
    uint32_t const scaled = quiet_for * (attempts - 1);
    quiet_for = scaled > PROXY_BOOTSEL_BACKOFF_MAX_MS
                    ? PROXY_BOOTSEL_BACKOFF_MAX_MS
                    : scaled;
  }
  if ((int32_t)(millis() - (idle_since + quiet_for)) < 0) {
    return;
  }
  if (g_bootsel_until) {
    return; // already holding the contact
  }
  attempts++;
  idle_since = millis();

  // Escalate to a full RP2040 reset once the gentler recovery has had a fair
  // go. This is not a shrug -- a reset is the ONE recovery measured to work
  // every time on a key that will not come back.
  //
  // Why it beats a cold entry is not established. See PROXY_BOOTSEL_RESET_AFTER
  // for the mechanisms already ruled out by measurement.
  if (attempts > PROXY_BOOTSEL_RESET_AFTER) {
    // Count resets in a scratch register, because `attempts` does NOT survive
    // one -- and escalating to a reset that clears the counter is a reset
    // loop. That is exactly what happened: two cold entries, reset, two more,
    // reset, forever, power-cycling the key the whole time. A counter in RAM
    // cannot fix this; it has to live somewhere the reset does not touch.
    //
    // Scratch[4] onwards is free for application use (the bootrom and SDK use
    // the lower ones), and it is preserved across a watchdog reset.
    uint32_t const resets = watchdog_hw->scratch[4];
    if (resets >= PROXY_BOOTSEL_MAX_RESETS) {
      LOGF("[bsel] %lu resets have not brought the key back -- stopping. "
           "Unplug and replug it, or press RESET.\r\n",
           (unsigned long)resets);
      g_bootsel_auto = false; // stop; a human has to look at this one
      return;
    }
    LOGF("[bsel] %u cold entries failed -- resetting the RP2040 (reset %lu of "
         "%u), which is the only recovery that reliably revives a dead key\r\n",
         attempts - 1, (unsigned long)(resets + 1),
         (unsigned)PROXY_BOOTSEL_MAX_RESETS);
    watchdog_hw->scratch[4] = resets + 1;
    for (uint8_t i = 0; i < 20 && !g_log_ring.empty(); i++) {
      drain_log();
      delay(5);
    }
    watchdog_reboot(0, 0, 0);
    return;
  }

  // Cold entry, not a pulse.
  //
  // This used to just tap the contact, up to PROXY_BOOTSEL_AUTO_MAX times, and
  // then give up permanently. It could never have worked: a Teensy samples its
  // program contact WHILE BOOTING, and a key whose application firmware has
  // been invalidated -- which is exactly the state we are recovering from --
  // has nothing running to notice a tap. Measured on a stuck key: three auto
  // pulses, three manual pulses and a plain VBUS cycle all did nothing, while
  // one cold entry recovered it immediately.
  //
  // Also no longer gives up. Stopping after three attempts left the rig
  // permanently stranded needing a human, which defeats the point of it. Retry
  // for as long as the port stays silent -- the interval already rate-limits
  // this to once per PROXY_BOOTSEL_AUTO_MS, and the log says what it is doing.
  LOGF("[bsel] silent %u ms -- cold bootloader entry (try %u, next in %u ms)\r\n",
       (unsigned)quiet_for, attempts,
       (unsigned)(attempts > 1 ? quiet_for * attempts : quiet_for));
  g_power_cmd = PWR_CMD_BOOTSEL_CYCLE; // core 1 performs it; VBUS is its to touch
#endif
}

// Deliberately many small LOGF calls: log_emit() formats through a 192-byte
// buffer, so this as one string printed the first three lines and cut the
// fourth mid-word.
static void print_help(void) {
  LOGF("commands:\r\n"
       "  B    COLD bootloader entry: contact held across a power cycle\r\n"
       "       (the only thing that recovers a key with no firmware)\r\n");
  LOGF("  b    tap contact -- only works on a key that is RUNNING firmware\r\n"
       "  G/g  ground the contact and HOLD / release it\r\n");
  LOGF("  A    toggle automatic recovery (off = no self power-cycling)\r\n"
       "  R    reboot RP2040 -> also a cold entry, via the reset pull-down\r\n");
  LOGF("  p    power-cycle host port      0 / 1   VBUS off / on\r\n"
       "  i    status                     h       host pins\r\n");
  // %-14s, not a hand-counted run of spaces: the second column starts at 34 on
  // every other line, and fixed padding sized for "(on)" shifts it when the
  // heartbeat is off.
  LOGF("  x    stored descriptors         d       live descriptor dump\r\n"
       "  t    3s heartbeat %-14s?       help\r\n",
       g_heartbeat ? "(on)" : "(off)");
  LOGF("  s    health line (for scripts)  z       zero counters\r\n"
       "  v    verbose PC->dev reports    U       RP2040 BOOTSEL (reflash me)\r\n");
  LOGF("  !1   wedge core 1 (watchdog)    !2      discard a report now\r\n"
       "  !3   PIO timeout in 6s          !4      failed interface clone\r\n"
       "  !5   discard a report in 6s\r\n");
}

// One-shot read of the host port pads, independent of the status block.
static void print_host_pins(void) {
  LOGF("raw pads GPIO%u=%u GPIO%u=%u  5V_EN=%u  (PIO driven, not a presence "
       "test)\r\n",
       (unsigned)PIN_USB_HOST_DP, gpio_get(PIN_USB_HOST_DP) ? 1u : 0u,
       (unsigned)PIN_USB_HOST_DP + 1, gpio_get(PIN_USB_HOST_DP + 1) ? 1u : 0u,
       gpio_get(PIN_5V_EN) ? 1u : 0u);
  LOGF("pad probe (pre-init, trustworthy): %s\r\n",
       g_pad_probe_text ? g_pad_probe_text : "(not run)");
}

static void console_service(void) {
  while (PROXY_LOG.available()) {
    int const key = PROXY_LOG.read();

    // Second byte of a fault-injection sequence. Consumed here so the digit
    // cannot also be read as a power command.
    if (g_fault_arm) {
      fault_select((char)key);
      continue;
    }

    switch (key) {
    case '!':
      g_fault_arm = true; // next byte selects the fault
      break;
    case 'p':
    case 'P':
      g_power_cmd = PWR_CMD_CYCLE;
      break;
    case '0':
      g_power_cmd = PWR_CMD_OFF;
      break;
    case '1':
      g_power_cmd = PWR_CMD_ON;
      break;
    case 'i':
    case 'I':
      print_status();
      break;
    case 'h':
    case 'H':
      print_host_pins();
      break;
    case 'v':
    case 'V':
      g_out_verbose = !g_out_verbose;
      LOGF("[out] verbose %s\r\n", g_out_verbose ? "ON" : "off");
      break;
    case 's':
    case 'S':
      print_health(); // machine-readable; scripts gate on this
      break;
    case 'z':
    case 'Z':
      reset_counters();
      break;
    case 'U':
      // Drop the RP2040 into its own ROM bootloader so a new proxy build can be
      // uploaded without anyone reaching for the BOOTSEL button. The rig is
      // meant to run unattended, and this was the last link in the chain that
      // still needed fingers.
      LOGF("[boot] entering RP2040 BOOTSEL -- copy firmware.uf2 to the drive\r\n");
      for (uint8_t i = 0; i < 20 && !g_log_ring.empty(); i++) {
        drain_log();
        delay(5);
      }
      reset_usb_boot(0, 0);
      break;
    case 't':
    case 'T':
      g_heartbeat = !g_heartbeat;
      LOGF("heartbeat %s\r\n", g_heartbeat ? "on" : "off");
      break;
    case 'd':
    case 'D':
      g_dump_req = true; // core 1 performs it; sync APIs need loop1 context
      break;
    case 'b':
      // Pulse the contact. Does NOTHING to a running key -- measured, including
      // a 3-second hold -- because the key samples this only at power-up. Kept
      // for the descriptor-less state, where it is what wakes the key up. To
      // enter the bootloader deliberately, use 'R'.
      bootsel_assert();
      g_bootsel_until = millis() + PROXY_BOOTSEL_HOLD_MS;
      LOGF("[bsel] contact held low for %u ms\r\n",
           (unsigned)PROXY_BOOTSEL_HOLD_MS);
      break;
    case 'B':
      // Cold entry: hold the contact across a power cycle. The only thing that
      // recovers a key whose application firmware is gone, because the contact
      // is sampled while booting and a tap on a running key needs firmware to
      // notice it. Use this, not 'b', when the port has gone silent.
      g_power_cmd = PWR_CMD_BOOTSEL_CYCLE;
      break;
    case 'G':
      // Ground the contact and HOLD it until 'g'. The key needs the contact
      // still grounded while it enumerates in order to offer its bootloader
      // descriptors, and any fixed timeout is a guess -- this makes the hold
      // explicit so the behaviour can be characterised rather than assumed.
      bootsel_assert();
      g_bootsel_until = 0; // no timeout: bootsel_service() will not release it
      g_bootsel_held = true;
      LOGF("[bsel] contact GROUNDED and held -- 'g' releases it\r\n");
      break;
    case 'g':
      bootsel_release();
      g_bootsel_held = false;
      LOGF("[bsel] contact released\r\n");
      break;
    case 'A':
      g_bootsel_auto = !g_bootsel_auto;
      LOGF("[bsel] automatic recovery %s\r\n",
           g_bootsel_auto ? "ON" : "off (board will not self-recover)");
      break;
    case 'R':
      // Reboot the RP2040 to put the attached key into ITS bootloader.
      //
      // Fallback for a key that is not responding at all. It is the recovery
      // measured to work every time on a descriptor-less key, where the VBUS
      // cold entry ('B') sometimes does not -- see PROXY_BOOTSEL_RESET_AFTER.
      //
      // During a reset GPIO18 goes undriven and GPIO6 reverts to
      // input-with-pull-down, so the key is power-cycled with its contact
      // grounded. Direct-wire only: with a transistor the same pull-down holds
      // the gate OFF, so the contact is released instead.
      LOGF("[bsel] rebooting RP2040 for key bootloader entry\r\n");
      delay(50); // let the log drain before the port disappears
      watchdog_reboot(0, 0, 0);
      break;
    case 'x':
    case 'X':
      // Re-print the descriptors captured at mount time. Needed because a mode
      // change (application <-> bootloader) re-enumerates our device too, so
      // the dump printed during the transition is lost with the console.
      for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
        host_link_t const *l = &g_link[i];
        if (!l->mounted || !l->desc_len) {
          continue;
        }
        LOGF("slot %u (%s), %u bytes:\r\n", i,
             l->known >= 0 ? k_itf[l->known].name : "UNKNOWN", l->desc_len);
        dump_descriptor(l->desc, l->desc_len);
      }
      break;
    case '?':
      print_help();
      break;
    default:
      break;
    }
  }
}

static void button_service(void) {
  static bool was_down = false;
  static uint32_t last_change = 0;

  bool const down = (digitalRead(PIN_BUTTON) == LOW);
  if (down != was_down && (millis() - last_change) > 40) {
    last_change = millis();
    was_down = down;
    if (down) {
      // Counted as well as logged: switching VBUS detaches the USB device, so
      // the log line below is usually lost before it can be drained to the CDC
      // console. The counter survives and shows up in the next status print.
      g_btn_presses++;
      g_btn_last_ms = millis();
      bool const turning_off = g_power_is_on;
      LOGF("[btn] press %lu -- VBUS %s\r\n", (unsigned long)g_btn_presses,
           turning_off ? "off" : "on");
      g_power_cmd = turning_off ? PWR_CMD_OFF : PWR_CMD_ON;
    }
  }
}

// --------------------------------------------------------------------------
// NeoPixel activity indicator
// --------------------------------------------------------------------------
//
// Colour identifies which interface last moved a report, so traffic can be told
// apart at a glance: keyboard white, FIDO2 yellow, raw HID green, seremu red.
// Dark when idle -- this is an activity light only. The onboard LED on GPIO 13
// carries board state (fault / host port off); the two never overlap in
// meaning.
//
// Driven from PIO1. Pico-PIO-USB owns PIO0 state machines 0-2, so pinning the
// pixel to PIO1 keeps it from ever competing with the USB host for a state
// machine or for instruction memory. If PIO1 is somehow unavailable the
// indicator silently disables itself rather than disturbing anything.

#define PROXY_PIXEL_BRIGHTNESS 40 // ceiling, 0-255: these are painfully bright
#define PROXY_PIXEL_PULSE_MS 160  // activity flash decay

static PIO const kPixelPio = pio1;
static uint kPixelSm = 0;
static bool g_pixel_ready = false;

// Stock WS2812 program from pico-examples: 10 state-machine cycles per bit.
static uint16_t const kWs2812Insns[] = {
    0x6221, // out x, 1   side 0 [2]
    0x1123, // jmp !x, 3  side 1 [1]
    0x1400, // jmp 0      side 1 [4]
    0xa442, // nop        side 0 [4]
};
static pio_program_t const kWs2812Program = {
    .instructions = kWs2812Insns,
    .length = 4,
    .origin = -1,       // let the assembler place it
    .pio_version = 0,   // RP2040 PIO; named to silence -Wmissing-field-initializers
};

// Set by whichever core forwarded a report. These are plain variable stores --
// core 1 never touches the PIO, the pixel, or any timing-sensitive code; it only
// records what just happened and core 0 renders it. A torn read between cores
// would at worst show the wrong colour for one 20 ms frame, so no locking is
// warranted.
static volatile int8_t g_pixel_itf = -1;
static volatile uint32_t g_pixel_at = 0;

static uint8_t const kItfColour[PROXY_MAX_HID][3] = {
    {255, 255, 255}, // keyboard: white
    {255, 200, 0},   // fido2:    yellow
    {0, 255, 0},     // rawhid:   green
    {255, 0, 0},     // seremu:   red
};

// seremu is the Teensy debug channel and chatters continuously (~27 reports/sec
// measured), so with plain last-writer-wins its red would overwrite every other
// colour within ~37 ms and nothing else would ever be visible. Ranking it below
// the rest means it still flashes red when the link is otherwise idle, but a
// keystroke, FIDO2 exchange or app message always shows through.
static uint8_t const kItfRank[PROXY_MAX_HID] = {
    2, // keyboard
    2, // fido2
    2, // rawhid
    1, // seremu
};

// Rank a slot by what it turned out to be. Unknown interfaces -- the bootloader
// -- rank highest: if firmware is being flashed, that is the only thing worth
// looking at.
static inline uint8_t slot_rank(uint8_t slot) {
  int8_t const known = g_link[slot].known;
  if (known < 0) {
    return 3;
  }
  return kItfRank[known];
}

static inline void pixel_note_activity(uint8_t slot) {
  if (slot >= PROXY_MAX_HID) {
    return;
  }
  uint32_t const now = millis();
  int8_t const cur = g_pixel_itf;
  if (cur >= 0 && cur < (int8_t)PROXY_MAX_HID &&
      (now - g_pixel_at) < PROXY_PIXEL_PULSE_MS &&
      slot_rank(slot) < slot_rank((uint8_t)cur)) {
    return; // don't let debug chatter paint over something more interesting
  }
  g_pixel_itf = (int8_t)slot;
  g_pixel_at = now;
}

// Cheap full-saturation hue sweep, 0-1535 around the wheel.
static void hue_to_rgb(uint16_t hue, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t const phase = (uint8_t)(hue / 256) % 6;
  uint8_t const step = (uint8_t)(hue % 256);
  switch (phase) {
  case 0: *r = 255; *g = step; *b = 0; break;
  case 1: *r = (uint8_t)(255 - step); *g = 255; *b = 0; break;
  case 2: *r = 0; *g = 255; *b = step; break;
  case 3: *r = 0; *g = (uint8_t)(255 - step); *b = 255; break;
  case 4: *r = step; *g = 0; *b = 255; break;
  default: *r = 255; *g = 0; *b = (uint8_t)(255 - step); break;
  }
}

static void pixel_begin(void) {
#ifdef NEOPIXEL_POWER
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
#endif

  // Ask before loading: depending on SDK version pio_add_program can panic
  // rather than return an error, and a hung board is a far worse outcome than
  // no status light.
  if (!pio_can_add_program(kPixelPio, &kWs2812Program)) {
    return;
  }
  int const offset = pio_add_program(kPixelPio, &kWs2812Program);
  if (offset < 0) {
    return;
  }
  int const sm = pio_claim_unused_sm(kPixelPio, false);
  if (sm < 0) {
    return;
  }
  kPixelSm = (uint)sm;

  pio_gpio_init(kPixelPio, PIN_NEOPIXEL);
  pio_sm_set_consecutive_pindirs(kPixelPio, kPixelSm, PIN_NEOPIXEL, 1, true);

  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, (uint)offset, (uint)offset + 3);
  sm_config_set_sideset(&c, 1, false, false);
  sm_config_set_sideset_pins(&c, PIN_NEOPIXEL);
  sm_config_set_out_shift(&c, false, true, 24); // MSB first, autopull at 24
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (800000.0f * 10.0f));
  pio_sm_init(kPixelPio, kPixelSm, (uint)offset, &c);
  pio_sm_set_enabled(kPixelPio, kPixelSm, true);

  g_pixel_ready = true;
}

static void pixel_put(uint8_t r, uint8_t g, uint8_t b) {
  if (!g_pixel_ready) {
    return;
  }
  // WS2812 wants GRB, MSB first, left-aligned for a 24-bit autopull.
  uint32_t const grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
  pio_sm_put_blocking(kPixelPio, kPixelSm, grb << 8u);
}

static void pixel_service(void) {
  static uint32_t next = 0;
  if (!g_pixel_ready || (int32_t)(millis() - next) < 0) {
    return;
  }
  next = millis() + 20; // 50 Hz is plenty and keeps the FIFO idle

  // Dark at rest: this pixel reports traffic only. Anything wrong (partial
  // match, PC not mounted, core 1 wedged) lights the onboard LED instead, so the
  // two indicators never compete for the same meaning.
  uint8_t r = 0, g = 0, b = 0;

  // Activity: fade from the interface colour back to dark.
  int8_t const slot = g_pixel_itf;
  uint32_t const since = millis() - g_pixel_at;
  if (slot >= 0 && slot < (int8_t)PROXY_MAX_HID &&
      since < PROXY_PIXEL_PULSE_MS) {
    uint32_t const fade = 255 - (since * 255 / PROXY_PIXEL_PULSE_MS);

    uint8_t cr, cg, cb;
    int8_t const known = g_link[slot].known;
    if (known >= 0) {
      cr = kItfColour[known][0];
      cg = kItfColour[known][1];
      cb = kItfColour[known][2];
    } else {
      // Unrecognised interface -- the bootloader. Cycle the hue continuously so
      // firmware traffic is unmistakable against the fixed colours above.
      static uint16_t hue = 0;
      hue = (uint16_t)((hue + 37) % 1536);
      hue_to_rgb(hue, &cr, &cg, &cb);
    }

    r = (uint8_t)((cr * fade + r * (255 - fade)) / 255);
    g = (uint8_t)((cg * fade + g * (255 - fade)) / 255);
    b = (uint8_t)((cb * fade + b * (255 - fade)) / 255);
  }

  pixel_put((uint8_t)(r * PROXY_PIXEL_BRIGHTNESS / 255),
            (uint8_t)(g * PROXY_PIXEL_BRIGHTNESS / 255),
            (uint8_t)(b * PROXY_PIXEL_BRIGHTNESS / 255));
}

// Core 0: has core 1 advanced since the last check?
static bool core1_alive(void) {
  static uint32_t last_ticks = 0;
  static uint32_t last_check = 0;
  static bool alive = true;

  if (millis() - last_check >= 500) {
    last_check = millis();
    alive = (g_core1_ticks != last_ticks);
    last_ticks = g_core1_ticks;
  }
  return alive;
}

// Core 0: reset the board if core 1 stops making progress.
//
// See PROXY_CORE1_WATCHDOG_MS for why this is not optional. Core 0 cannot
// simply restart the host stack -- the wedge is a spin inside Pico-PIO-USB on
// core 1, so core 1 is never coming back to be told anything, and its PIO state
// machines and DMA channels are left mid-transfer. A full reset is the only
// state we can actually reach from here.
//
// Deliberately fires even with nothing attached: a wedge with an empty port is
// still a wedge, and recovering quietly beats sitting dead with the LED lit.
static void core1_watchdog_service(void) {
  static uint32_t last_ticks = 0;
  static uint32_t last_progress = 0;

  if (g_core1_fatal) {
    return; // already reported itself; leave the LED lit rather than loop-reset
  }

  uint32_t const now = millis();
  if (g_core1_ticks != last_ticks) {
    last_ticks = g_core1_ticks;
    last_progress = now;
    return;
  }
  if (!last_progress) {
    last_progress = now; // first call: start the clock, do not judge yet
    return;
  }

  // Some work legitimately blocks core 1 for longer than the timeout: a
  // descriptor dump uses synchronous transfers, and a cold bootloader entry
  // deliberately sits through a power cycle. Neither is a wedge.
  if (g_dump_req || g_core1_long_op) {
    last_progress = now;
    return;
  }

  if ((uint32_t)(now - last_progress) < PROXY_CORE1_WATCHDOG_MS) {
    return;
  }

  uint8_t const ph = g_core1_phase;
  LOGF("[wdog] core 1 stalled %u ms in %s -- resetting\r\n",
       (unsigned)(now - last_progress),
       ph < (sizeof(kPhaseName) / sizeof(kPhaseName[0])) ? kPhaseName[ph] : "?");
  LOGF("[wdog] PC->dev %lu sent / %lu dropped -- anything dropped was ACKed to "
       "the PC as success\r\n",
       (unsigned long)g_out_sent, (unsigned long)g_out_dropped);
  // Push it out ourselves: the reboot is immediate and loop() will not get
  // another chance to drain the ring.
  for (uint8_t i = 0; i < 20 && !g_log_ring.empty(); i++) {
    drain_log();
    delay(5);
  }
  watchdog_reboot(0, 0, 0);
}

// The onboard LED on GPIO 13 means three things and only three:
//
//   dark     working normally, or simply nothing plugged into the host port
//   flashing host port powered down -- proof the board itself is still alive,
//            which matters because cutting VBUS also detaches us from the PC,
//            so every other sign of life disappears at the same time
//   lit      something needs attention
//
// An empty host port is not a fault, so no key attached means dark.
//
// Faults are:
//   - core 1 wedged, or the host stack never started
//   - a PC->device report was dropped after we had already ACKed it
//   - a key is attached but not all four interfaces matched
//   - all four matched but the PC has not mounted the proxy
static bool led_fault(void) {
  if (g_core1_fatal || !core1_alive()) {
    return true;
  }
  if (g_out_drop_flag) {
    return true; // sticky until reset: the PC was told a lie and cannot be told
  }
  if (g_clone_failures) {
    return true; // an interface the key offered is missing from what the PC sees
  }
  if (!g_power_is_on) {
    return false; // VBUS deliberately cut; nothing is expected to be up
  }

  uint8_t linked = 0;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted) {
      linked++;
    }
  }
  if (linked == 0) {
    return false; // empty port is a normal resting state, not a problem
  }
  // Compare against what the host stack actually bound, NOT PROXY_MAX_HID: a
  // bootloader legitimately exposes a single interface, and demanding four made
  // a perfectly healthy key look like a fault.
  uint8_t const bound = g_host_hid_count;
  if (bound && linked < bound) {
    return true; // an interface the host bound failed to clone
  }
#if PROXY_DIAG
  // Diagnostic builds never present anything to the PC, so stop here.
  return false;
#else
  return !(g_pc_attached && TinyUSBDevice.mounted());
#endif
}

// A fault has to persist before it lights the LED: enumeration, the settle
// window and the deliberate power cycles all pass briefly through states that
// look faulty, and flickering on those would train you to ignore the light.
#define PROXY_LED_FAULT_MS 2000

// Half-period of the "host port is off" heartbeat.
#define PROXY_LED_FLASH_MS 400

static void led_service(void) {
  static uint32_t changed_at = 0;
  static uint32_t next_toggle = 0;
  static bool last_fault = false;
  static bool on = false;

  bool const fault = led_fault();
  if (fault != last_fault) {
    last_fault = fault;
    changed_at = millis();
  }

  // A real fault outranks the heartbeat: steady beats blinking for "look here".
  if (fault && (millis() - changed_at) >= PROXY_LED_FAULT_MS) {
    if (!on) {
      on = true;
      digitalWrite(PIN_LED, HIGH);
    }
    return;
  }

  if (!g_power_is_on) {
    if ((int32_t)(millis() - next_toggle) >= 0) {
      next_toggle = millis() + PROXY_LED_FLASH_MS;
      on = !on;
      digitalWrite(PIN_LED, on);
    }
    return;
  }

  if (on) {
    on = false;
    digitalWrite(PIN_LED, LOW);
  }
}

// Defined below with the rest of the device-side presence handling.
static void usb_present(bool with_hid);
extern bool g_usb_has_hid;

void setup() {
  PROXY_LOG.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pixel_begin(); // core 0 only -- see the note above pixel_service()
  bootsel_release(); // high-Z before anything else can drive it

  // Deliberately no wait for the port to be opened, and no keypress gate. Both
  // depended on tud_cdc_connected(), i.e. on the host asserting DTR, which made
  // the board silent unless the terminal happened to raise it. Diagnostic mode
  // re-runs the whole sequence on a timer instead (see loop()), so a monitor
  // attached at any moment gets a complete trace without touching a key.
  g_log_ready = true; // releases core 1

  LOGF("\r\n=== Feather RP2040 USB HID proxy ===\r\n");
  LOGF("clock %lu Hz, diag=%u\r\n", (unsigned long)F_CPU, (unsigned)PROXY_DIAG);

#if PROXY_DIAG
  LOGF("[dev] DIAGNOSTIC MODE: USB config left alone, nothing is proxied\r\n");
#else
  // arduino-pico has already brought the device stack up with a CDC-only
  // configuration, so detach before touching the descriptors.
  TinyUSBDevice.detach();
  delay(10);

  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    g_hid[i].setReportCallback(nullptr, k_set_cb[i]);
    // Placeholder only, so begin() below builds a well-formed descriptor. The
    // real report descriptor is captured from the attached device and installed
    // by usb_build_config() on every rebuild.
    g_hid[i].setReportDescriptor(k_itf[0].desc, k_itf[0].desc_len);

    // begin() once, purely to claim this object's HID instance index; the
    // configuration it appends to here is thrown away immediately below. Every
    // later rebuild goes through addInterface(), because begin() would
    // early-return now that the instance is valid.
    if (!g_hid[i].begin()) {
      LOGF("[dev] FAILED to claim HID instance %u\r\n", i);
    }
  }

#if PROXY_ENABLE_CDC
  // Come up console-only. The HID interfaces are added when the key links, and
  // removed again whenever it goes away, so the PC sees a real unplug while the
  // console survives to switch the host port back on.
  usb_present(false);
  LOGF("[dev] %04X:%04X up as console-only, waiting for key\r\n", PROXY_VID,
       PROXY_PID);
#else
  // No console to keep alive: stay off the bus until the key appears.
  g_usb_has_hid = false;
  LOGF("[dev] configured %04X:%04X with %u HID interfaces, waiting for key\r\n",
       PROXY_VID, PROXY_PID, PROXY_MAX_HID);
#endif
#endif // PROXY_DIAG

  print_status();
  print_help();
}

// Rebuild the USB configuration from scratch. Core 0 only, and only while
// detached -- the host must never see the descriptors change underneath it.
//
// with_hid=false yields a console-only device: the four HID interfaces vanish
// from the PC exactly as an unplug would look, while the CDC stays so the board
// can still be talked to and switched back on.
//
// Note that Adafruit_USBD_HID::begin() and Adafruit_USBD_CDC::begin() both
// early-return once their instance is valid, so they cannot be reused here;
// addInterface() is the only way to re-add an already-initialised interface to
// a freshly cleared configuration.
static void usb_build_config(bool with_hid) {
  TinyUSBDevice.clearConfiguration();

  // Adopt whatever is attached, so bootloader mode is presented to the PC as
  // the bootloader. A flashing tool matches on VID/PID; showing the application
  // firmware's IDs while the key is in its bootloader would make it invisible.
  uint16_t const vid = g_dev_vid ? g_dev_vid : PROXY_VID;
  uint16_t const pid = g_dev_pid ? g_dev_pid : PROXY_PID;
  TinyUSBDevice.setID(vid, pid);
  TinyUSBDevice.setManufacturerDescriptor(PROXY_MANUFACTURER);
  TinyUSBDevice.setProductDescriptor(PROXY_PRODUCT);
  TinyUSBDevice.setSerialDescriptor(PROXY_SERIAL);

  // HID goes FIRST and the console last, so the cloned interfaces land on the
  // same interface numbers as the real device (rawhid at MI_02 on an OnlyKey).
  // Host software routinely locates a raw-HID endpoint by interface number, and
  // with the console at MI_00/01 everything shifted up by two -- an app looking
  // for MI_02 found the keyboard instead and silently talked to nothing.
  // Windows binds the CDC by class and IAD, so its position does not matter.
  if (with_hid) {
    // Mirror exactly the interfaces that are live, using the descriptors
    // captured from the device.
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      host_link_t const *link = &g_link[i];
      if (!link->mounted || !link->desc_len) {
        continue;
      }
      g_hid[i].setReportDescriptor(link->desc, link->desc_len);
      g_hid[i].setBootProtocol(link->known >= 0
                                   ? k_itf[link->known].boot_protocol
                                   : (uint8_t)HID_ITF_PROTOCOL_NONE);
      g_hid[i].setPollInterval(1);
      // Mirror the device: only offer an interrupt OUT endpoint if it has one.
      g_hid[i].enableOutEndpoint(link->has_out_ep);
      if (!TinyUSBDevice.addInterface(g_hid[i])) {
        LOGF("[dev] FAILED to add HID interface %u\r\n", i);
      }
      }
  }

#if PROXY_ENABLE_CDC
  TinyUSBDevice.addInterface(Serial); // console last -- see the note above
#endif
}

// Tracks what the PC is currently being shown, as opposed to what core 1 wants.
bool g_usb_has_hid = false;

static void usb_present(bool with_hid) {
  TinyUSBDevice.detach();
  delay(20); // let the host see the disconnect before the descriptors change
  usb_build_config(with_hid);
  delay(10);
  TinyUSBDevice.attach();
  g_usb_has_hid = with_hid;
}

// Core 0 owns the device stack: apply whatever presence core 1 published.
static void presence_service(void) {
  bool const want = g_pc_present_req;
  if (want == g_usb_has_hid) {
    return;
  }

  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    g_inq[i].flush();
  }

#if PROXY_ENABLE_CDC
  LOGF("[dev] re-enumerating %s HID interfaces\r\n", want ? "with" : "WITHOUT");
  usb_present(want);
#else
  // No console to preserve, so simply drop off the bus entirely.
  if (want) {
    LOGF("[dev] attaching to PC\r\n");
    usb_present(true);
  } else {
    LOGF("[dev] detaching from PC\r\n");
    TinyUSBDevice.detach();
    g_usb_has_hid = false;
  }
#endif
  g_pc_attached = want;
}

// Forward device -> PC. Each interface drains independently, so one that the
// host has stopped polling cannot hold up the others.
static void forward_in(void) {
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    while (g_hid[i].ready()) {
      in_report_t *rpt = g_inq[i].peek();
      if (!rpt) {
        break;
      }
      if (!g_hid[i].sendReport(0, rpt->data, rpt->len)) {
        break; // leave it queued and retry on the next pass
      }
      g_inq[i].pop();
      g_in_sent++;
      pixel_note_activity(i);
    }
  }
}

void loop() {
#if PROXY_DIAG
  // Self-driving: print the state and force a fresh enumeration on a timer, so
  // whenever a monitor is attached it sees a full cycle within a few seconds
  // without any interaction.
  static uint32_t next_cycle = 4000;
  if ((int32_t)(millis() - next_cycle) >= 0) {
    next_cycle = millis() + PROXY_DIAG_CYCLE_MS;
    LOGF("\r\n######## diag cycle @ %lu ms ########\r\n",
         (unsigned long)millis());
    print_status();

    // Only force a re-enumeration when nothing is linked. Power-cycling a
    // device that is already up would be actively destructive here: it drops
    // the OnlyKey out of bootloader mode, which is exactly the state we need to
    // hold still while diagnosing.
    bool linked = false;
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      linked = linked || g_link[i].mounted;
    }
    if (linked) {
      LOGF("[diag] device linked -- not power-cycling\r\n");
    } else {
      LOGF("[diag] nothing linked, forcing re-enumeration...\r\n");
      g_power_cmd = PWR_CMD_CYCLE;
    }
  }

  static uint32_t next_beat = 0;
  if (g_heartbeat && (int32_t)(millis() - next_beat) >= 0) {
    next_beat = millis() + 3000;
    print_status();
  }
#else
  presence_service();
  forward_in();
#endif

  drain_log();
  bootsel_service();
  console_service();
  button_service();
  led_service();
  pixel_service(); // core 0 only: the sole place the pixel PIO is touched
  fault_service();
  core1_watchdog_service(); // last: it may not return
}

// --------------------------------------------------------------------------
// Core 1 -- PIO USB host stack
// --------------------------------------------------------------------------

// Core 1: start polling an interface only once it has had time to settle after
// enumerating. Kept out of the mount callback on purpose -- issuing the first IN
// request from inside the callback hits the device at its busiest moment.
static void arm_service(void) {
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    host_link_t *link = &g_link[i];
    if (!link->mounted) {
      continue;
    }
    if ((int32_t)(millis() - (link->mounted_at + PROXY_ARM_DELAY_MS)) < 0) {
      continue; // still settling after enumeration
    }

    // Keep a request outstanding at all times rather than arming once. An
    // earlier version armed a single time per mount and relied on the report
    // callback to re-arm; a single failed re-arm then silenced that interface
    // permanently, which is exactly what stalled forwarding after a few hundred
    // reports. receive_ready() is false while a request is already pending, so
    // this only re-issues when the chain has actually been broken.
    if (!tuh_hid_receive_ready(link->daddr, link->idx)) {
      continue;
    }

    // Never request data we have nowhere to put. Once a report has been
    // received there is no way to refuse it -- dropping it is silent loss, the
    // same failure that corrupted firmware writes on the OUT path. Simply not
    // asking leaves the report in the device, which holds it until we are
    // ready: real USB flow control instead of a lossy queue.
    //
    // reserve() has no side effects, so this is a pure "is there room" probe.
    // Only one request is outstanding per interface, so one free slot is
    // exactly enough.
    if (!g_inq[i].reserve()) {
      continue;
    }
    if (tuh_hid_receive_report(link->daddr, link->idx)) {
      if (!link->armed) {
        link->armed = true;
        LOGF("[host] slot %u armed after %u ms\r\n", i,
             (unsigned)PROXY_ARM_DELAY_MS);
      } else {
        link->rearms++;
      }
    }
  }
}

// Once mounts/umounts have stopped arriving, publish presence to core 0.
static void link_service(void) {
  if (!g_link_settle_at || (int32_t)(millis() - g_link_settle_at) < 0) {
    return;
  }
  g_link_settle_at = 0;

  uint8_t linked = 0;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted) {
      linked++;
    }
  }
  LOGF("[host] %u of %u interfaces linked\r\n", linked, PROXY_MAX_HID);
  pc_present_set(linked > 0);
}

// Forward PC -> device. Interrupt OUT where the device offers one, control
// SET_REPORT otherwise (keyboard LEDs) or for Feature reports. Both send paths
// return false while busy, which doubles as the gate against overlapping
// transfers: leave the report queued and retry next pass.
// Core 1 only. Set after a HalfKay block-0 write; nothing is forwarded to that
// interface until the chip erase it kicked off has had time to finish.
static uint32_t g_out_hold_until[PROXY_MAX_HID];

// Does this look like the HalfKay write that erases the chip? Only the
// bootloader's own interface qualifies (known < 0 means no descriptor matched),
// and only a firmware-sized block -- an ordinary report starting 00 00 00 is
// commonplace and must not trigger a three-second stall. The reboot command is
// FF FF FF, so it is never confused with this.
static bool is_halfkay_erase_block(host_link_t const *link,
                                   out_report_t const *rpt) {
  return link->known < 0 && rpt->len > PROXY_SMALL_REPORT_MAX &&
         rpt->data[0] == 0 && rpt->data[1] == 0 && rpt->data[2] == 0;
}

static void forward_out(void) {
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    host_link_t const *link = &g_link[i];

    // Still inside a chip-erase window: the device NAKs everything, so sending
    // now only produces retries that hammer a busy bootloader. Abandoned
    // immediately if the device goes away, so an unplug during the erase does
    // not leave the queue frozen for three seconds.
    if (g_out_hold_until[i]) {
      if (!link->mounted) {
        g_out_hold_until[i] = 0;
      } else if ((int32_t)(millis() - g_out_hold_until[i]) < 0) {
        continue;
      } else {
        g_out_hold_until[i] = 0;
        LOGF("[out] slot %u erase window over, resuming\r\n", i);
      }
    }

    while (true) {
      out_report_t *rpt = g_outq[i].peek();
      if (!rpt) {
        break;
      }

      if (!link->mounted) {
        // Nothing to forward to -- discard rather than stall the queue.
        g_outq[i].pop();
        g_out_dropped++;
        g_out_hold_until[i] = 0;
        continue;
      }

      bool ok;
      if (link->has_out_ep && rpt->report_type == HID_REPORT_TYPE_OUTPUT) {
        if (!tuh_hid_send_ready(link->daddr, link->idx)) {
          break;
        }
        ok = tuh_hid_send_report(link->daddr, link->idx, rpt->report_id,
                                 rpt->data, rpt->len);
      } else {
        ok = tuh_hid_set_report(link->daddr, link->idx, rpt->report_id,
                                rpt->report_type, rpt->data, rpt->len);
      }

      if (!ok) {
        break; // busy: leave it queued and retry on the next pass
      }

      // Logged on success, not on attempt. Logging before the send re-printed
      // the same report on every retry -- 490 lines for 210 delivered blocks --
      // which made a capture almost unreadable and looked like duplicate
      // traffic. One line here means exactly one report actually delivered.
      if (g_out_verbose) {
        LOGF("[out] slot %u id=%u type=%u len=%u : %02X %02X %02X %02X "
             "%02X %02X %02X %02X\r\n",
             i, rpt->report_id, rpt->report_type, rpt->len,
             rpt->len > 0 ? rpt->data[0] : 0, rpt->len > 1 ? rpt->data[1] : 0,
             rpt->len > 2 ? rpt->data[2] : 0, rpt->len > 3 ? rpt->data[3] : 0,
             rpt->len > 4 ? rpt->data[4] : 0, rpt->len > 5 ? rpt->data[5] : 0,
             rpt->len > 6 ? rpt->data[6] : 0, rpt->len > 7 ? rpt->data[7] : 0);
      }

      bool const erase = is_halfkay_erase_block(link, rpt);
      g_outq[i].pop();
      g_out_sent++;
      pixel_note_activity(i); // variable store only; core 0 does the rendering

      if (erase) {
        g_out_hold_until[i] = millis() + PROXY_HALFKAY_ERASE_MS;
        LOGF("[out] slot %u block 0 sent, holding %u ms for chip erase\r\n", i,
             (unsigned)PROXY_HALFKAY_ERASE_MS);
        break; // nothing else may go out until the erase finishes
      }
    }
  }
}

// Core 1: read the device's descriptors directly, bypassing whatever the
// enumeration path did or did not manage to parse. This is the ground truth for
// "why did no class driver bind".
static void dump_device(uint8_t daddr) {
  static uint8_t buf[1024]; // deliberately larger than CFG_TUH_ENUMERATION_BUFSIZE

  LOGF("==== descriptor dump, addr %u ====\r\n", daddr);

  tusb_desc_device_t dd;
  if (tuh_descriptor_get_device_sync(daddr, &dd, sizeof(dd)) ==
      XFER_RESULT_SUCCESS) {
    LOGF("device : USB %04X, class %02X/%02X/%02X, %04X:%04X, %u config(s), "
         "ep0 %u\r\n",
         dd.bcdUSB, dd.bDeviceClass, dd.bDeviceSubClass, dd.bDeviceProtocol,
         dd.idVendor, dd.idProduct, dd.bNumConfigurations, dd.bMaxPacketSize0);
  } else {
    LOGF("device : GET_DESCRIPTOR(device) FAILED\r\n");
    return;
  }

  // Read just the 9-byte header first to learn wTotalLength, then the whole
  // thing. If wTotalLength exceeds CFG_TUH_ENUMERATION_BUFSIZE, enumeration
  // cannot have completed -- usbh.c asserts on exactly that.
  if (tuh_descriptor_get_configuration_sync(daddr, 0, buf, 9) !=
      XFER_RESULT_SUCCESS) {
    LOGF("config : GET_DESCRIPTOR(config header) FAILED\r\n");
    return;
  }
  uint16_t const total = ((tusb_desc_configuration_t *)buf)->wTotalLength;
  uint8_t const nitf = ((tusb_desc_configuration_t *)buf)->bNumInterfaces;
  LOGF("config : wTotalLength %u, bNumInterfaces %u, enum buffer %u -> %s\r\n",
       total, nitf, (unsigned)CFG_TUH_ENUMERATION_BUFSIZE,
       (total > CFG_TUH_ENUMERATION_BUFSIZE) ? "TOO BIG, ENUMERATION ABORTS"
                                             : "fits");

  uint16_t const want = (total > sizeof(buf)) ? sizeof(buf) : total;
  if (tuh_descriptor_get_configuration_sync(daddr, 0, buf, want) !=
      XFER_RESULT_SUCCESS) {
    LOGF("config : full GET_DESCRIPTOR FAILED\r\n");
    return;
  }

  LOGF("config raw:\r\n");
  for (uint16_t i = 0; i < want; i++) {
    LOGF("%02X", buf[i]);
    if ((i % 32) == 31) {
      LOGF("\r\n");
    }
  }
  LOGF("\r\n");

  // Walk the interface descriptors so the classes are readable at a glance.
  for (uint16_t i = 0; i + 1 < want;) {
    uint8_t const len = buf[i];
    uint8_t const type = buf[i + 1];
    if (len < 2) {
      break;
    }
    if (type == TUSB_DESC_INTERFACE && i + 8 < want) {
      LOGF("  itf %u: alt %u, %u ep, class %02X/%02X/%02X\r\n", buf[i + 2],
           buf[i + 3], buf[i + 4], buf[i + 5], buf[i + 6], buf[i + 7]);
    }
    i += len;
  }
  LOGF("==== end dump ====\r\n");
}

void setup1() {
  // VBUS on before the host stack starts looking for a device.
  pinMode(PIN_5V_EN, OUTPUT);
  power_apply(true);

  // Core 0 opens the log port; wait for it so nothing below is lost. In
  // diagnostic mode wait forever, so nothing starts until a terminal is
  // attached and has released the gate.
#if PROXY_DIAG
  while (!g_log_ready) {
    delay(1);
  }
#else
  for (uint32_t deadline = millis() + 3000;
       !g_log_ready && (int32_t)(millis() - deadline) < 0;) {
    delay(1);
  }
#endif

  // PIO USB bit-banging needs a system clock that is a multiple of 12 MHz.
  // 12 MHz alignment is necessary but not sufficient: 120 MHz passes this check
  // and still fails to enumerate the OnlyKey. See the note on board_build.f_cpu.
  uint32_t const cpu_hz = clock_get_hz(clk_sys);
  LOGF("[host] sys clock %lu Hz, D+ on GPIO%u, 5V_EN on GPIO%u\r\n",
       (unsigned long)cpu_hz, (unsigned)PIN_USB_HOST_DP, (unsigned)PIN_5V_EN);
  if (cpu_hz % 12000000UL) {
    LOGF("[host] FATAL: clock is not a multiple of 12 MHz; "
         "set board_build.f_cpu = 120000000L\r\n");
    g_core1_fatal = true;
    while (true) {
      delay(1000);
    }
  }

  // Probe the pair BEFORE handing the pins to PIO. With both pads as inputs
  // with pull-downs, an attached device's own 1.5k pull-up is the only thing
  // that can pull a line high, so this says unambiguously which GPIO carries
  // D+ -- something that cannot be inferred once PIO is driving the lines.
  gpio_init(PIN_USB_HOST_DP);
  gpio_set_dir(PIN_USB_HOST_DP, GPIO_IN);
  gpio_pull_down(PIN_USB_HOST_DP);
  gpio_init(PIN_USB_HOST_DP + 1);
  gpio_set_dir(PIN_USB_HOST_DP + 1, GPIO_IN);
  gpio_pull_down(PIN_USB_HOST_DP + 1);
  delay(300); // let VBUS come up and the device assert its pull-up

  bool const hi_16 = gpio_get(PIN_USB_HOST_DP);
  bool const hi_17 = gpio_get(PIN_USB_HOST_DP + 1);
  g_pad_probe_text = (hi_16 && !hi_17) ? "D+ on the first pin (normal DPDM)"
                     : (!hi_16 && hi_17)
                         ? "D+ on the SECOND pin (pair is SWAPPED)"
                     : (!hi_16 && !hi_17)
                         ? "neither pulled up: nothing attached, or no VBUS"
                         : "both high: unexpected";
  LOGF("[host] pad probe: GPIO%u=%u GPIO%u=%u -> %s\r\n",
       (unsigned)PIN_USB_HOST_DP, hi_16 ? 1u : 0u,
       (unsigned)PIN_USB_HOST_DP + 1, hi_17 ? 1u : 0u, g_pad_probe_text);

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;

#if defined(PROXY_SWAP_DP_DM) && PROXY_SWAP_DP_DM
  // Forced swap: D+ is the higher pin, D- the lower one.
  pio_cfg.pin_dp = PIN_USB_HOST_DP + 1;
  pio_cfg.pinout = PIO_USB_PINOUT_DMDP; // D- = D+ - 1
  LOGF("[host] pinout forced to DMDP: D+=GPIO%u D-=GPIO%u\r\n",
       (unsigned)PIN_USB_HOST_DP + 1, (unsigned)PIN_USB_HOST_DP);
#else
  pio_cfg.pin_dp = PIN_USB_HOST_DP;
  pio_cfg.pinout = PIO_USB_PINOUT_DPDM; // D- = D+ + 1
  LOGF("[host] pinout DPDM: D+=GPIO%u D-=GPIO%u\r\n",
       (unsigned)PIN_USB_HOST_DP, (unsigned)PIN_USB_HOST_DP + 1);
#endif

  USBHost.configure_pio_usb(1, &pio_cfg);

  // Running begin() on core 1 keeps the bit-banging work off core 0.
  if (!USBHost.begin(1)) {
    LOGF("[host] FATAL: USBHost.begin(1) failed\r\n");
    g_core1_fatal = true;
    while (true) {
      delay(1000);
    }
  }
  g_host_started = true;
  LOGF("[host] PIO USB host started, waiting for a device\r\n");
}

void loop1() {
  // Fault injection: stop advancing the tick counter, exactly as a real wedge
  // inside USBHost.task() does. Core 0 keeps running and answering the PC,
  // which is what made the real thing so hard to spot.
  while (g_fault_wedge) {
    tight_loop_contents();
  }

  g_core1_ticks++; // liveness, watched by core 0
  g_core1_phase = C1_HOST_TASK;
  USBHost.task();
  g_core1_phase = C1_POWER;
  power_service();
  g_core1_phase = C1_LINK;
  arm_service();
  link_service();

  g_core1_phase = C1_DUMP;
  if (g_dump_req) {
    g_dump_req = false;
    uint8_t const daddr = g_dev_addr;
    if (daddr) {
      dump_device(daddr);
    } else {
      LOGF("[dump] no device enumerated\r\n");
    }
  }

  // Publish what the HID host driver has actually bound, so core 0 can report
  // it without touching tuh_*.
  // Deliberately NOT calling tuh_hid_itf_get_total_count() here. It reaches
  // into the HID host driver's interface table from outside the normal task
  // flow, and core 1 reproducibly wedged inside this exact window while the
  // driver was tearing those entries down during a device mode switch
  // (application <-> bootloader). The count is cosmetic, so derive it from
  // state we own instead of touching the driver's.
  g_core1_phase = C1_POLL;
  uint8_t bound = 0;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted) {
      bound++;
    }
  }
  g_host_hid_count = bound;
  g_core1_phase = C1_IDLE;

#if !PROXY_DIAG
  g_core1_phase = C1_FORWARD_OUT;
  forward_out();
#endif
}

// --------------------------------------------------------------------------
// TinyUSB host callbacks -- all run on core 1
// --------------------------------------------------------------------------

extern "C" {

// Device-level attach: fires as soon as enumeration succeeds, before any class
// driver binds. If this never prints, the problem is the host port or the cable
// rather than anything HID-related.
void tuh_mount_cb(uint8_t daddr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  g_dev_addr = daddr;
  LOGF("[host] device attached: addr %u, %04X:%04X\r\n", daddr, vid, pid);
}

void tuh_umount_cb(uint8_t daddr) {
  if (g_dev_addr == daddr) {
    g_dev_addr = 0;
  }
  // Clone failures describe the attachment that just went away, so clear them
  // here rather than letting a previous device's problems be reported against
  // the next one.
  g_clone_failures = 0;
  LOGF("[host] device detached: addr %u\r\n", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc_report,
                      uint16_t desc_len) {
  g_core1_phase = C1_MOUNT_CB;
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  LOGF("[host] HID mounted: addr %u idx %u, %04X:%04X, %u-byte descriptor\r\n",
       daddr, idx, vid, pid, desc_len);

  if (!desc_report || !desc_len) {
    // Descriptors larger than CFG_TUH_ENUMERATION_BUFSIZE are not delivered.
    LOGF("[host] no report descriptor available -- cannot map interface\r\n");
    g_clone_failures++;
    return;
  }

#if PROXY_DIAG
  // Always show the bytes in diagnostic mode, matched or not.
  dump_descriptor(desc_report, desc_len);
#endif

  if (desc_len > PROXY_MAX_DESC_LEN) {
    LOGF("[host] descriptor %u bytes exceeds %u -- cannot clone\r\n", desc_len,
         (unsigned)PROXY_MAX_DESC_LEN);
    g_clone_failures++;
    return;
  }

  // Slots are filled in mount order, not by interface identity: an unknown
  // device (the bootloader) has to be proxied just as readily as a known one.
  int8_t slot = -1;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (!g_link[i].mounted) {
      slot = (int8_t)i;
      break;
    }
  }
  if (slot < 0) {
    LOGF("[host] no free slot for addr %u idx %u -- raise PROXY_MAX_HID\r\n",
         daddr, idx);
    g_clone_failures++;
    return;
  }

  tuh_itf_info_t info;
  bool const has_out_ep =
      tuh_hid_itf_get_info(daddr, idx, &info) && info.desc.bNumEndpoints >= 2;
  int8_t const known = match_interface(desc_report, desc_len);

  host_link_t *link = &g_link[slot];
  link->daddr = daddr;
  link->idx = idx;
  link->has_out_ep = has_out_ep;
  link->known = known;
  link->desc_len = desc_len;
  memcpy(link->desc, desc_report, desc_len);
  link->mounted_at = millis();
  link->armed = false;
  link->in_count = 0;
  link->rearms = 0; // arm_service() polls it once it has had time to settle
  __dmb();             // descriptor must be visible on core 0 before it goes live
  link->mounted = true;

  g_dev_vid = vid;
  g_dev_pid = pid;
  g_link_settle_at = millis() + PROXY_SETTLE_MS;

  LOGF("[host] slot %u <- %s, %u-byte descriptor, OUT endpoint: %s\r\n", slot,
       known >= 0 ? k_itf[known].name : "UNKNOWN (cloning verbatim)", desc_len,
       has_out_ep ? "yes" : "control only");
  if (known < 0) {
    LOGF("[map] unrecognised, cloned verbatim (%u bytes):\r\n", desc_len);
    dump_descriptor(desc_report, desc_len);
  }

  // Deliberately not armed here -- see PROXY_ARM_DELAY_MS.
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
  g_core1_phase = C1_UMOUNT_CB;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted && g_link[i].daddr == daddr && g_link[i].idx == idx) {
      g_link[i].mounted = false;
      g_link_settle_at = millis() + PROXY_SETTLE_MS;
      LOGF("[host] itf %u (%s) unmounted\r\n", i, k_itf[i].name);
    }
  }
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                uint8_t const *report, uint16_t len) {
  g_core1_phase = C1_REPORT_CB;
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted && g_link[i].daddr == daddr && g_link[i].idx == idx) {
      g_link[i].in_count++;
      in_report_t *slot = g_inq[i].reserve();
      if (!slot) {
        // Should now be unreachable: arm_service() only requests a report when
        // there is a slot free, and nothing else arms this interface. If this
        // ever fires again, the flow control has been broken somewhere.
        g_in_dropped++;
      } else {
        if (len > sizeof(slot->data)) {
          len = sizeof(slot->data);
        }
        slot->len = (uint8_t)len;
        memcpy(slot->data, report, len);
        g_inq[i].commit();
      }
      break;
    }
  }

  // Deliberately NOT re-arming here.
  //
  // This used to call tuh_hid_receive_report() unconditionally, which quietly
  // defeated the flow control in arm_service(). That function refuses to ask
  // for a report unless a queue slot is free -- the whole point being that an
  // unrequested report stays in the device, which is real USB back-pressure,
  // whereas a received one can only be dropped. Re-arming here asked for the
  // next report regardless, so a queue that could not drain (the PC detached
  // mid-transition, say) overflowed and lost reports. Measured: 12 lost across
  // one test run.
  //
  // The comment this replaces claimed the interface goes silent without it.
  // That was true when arm_service() armed only once per mount, but it now
  // keeps a request outstanding at all times, so it re-arms as soon as there is
  // somewhere to put the data. tuh_hid_receive_ready() is false while a request
  // is pending, so the two cannot race.
}

} // extern "C"
