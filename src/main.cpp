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
#include <hardware/sync.h>

// pio_usb.h must come first: its presence is what puts tusb_config into
// PIO-host mode when Adafruit_TinyUSB.h is pulled in.
#include "pio_usb.h"

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

// Ceiling on an explicit "VBUS off" when the console rides on USB. See the
// PWR_CMD_OFF case for why leaving it off indefinitely would be a trap.
#define PROXY_POWER_OFF_MAX_MS 30000

// Diagnostic mode re-runs status + a power cycle on this period, so a monitor
// attached at any time sees a complete enumeration attempt without interaction.
#define PROXY_DIAG_CYCLE_MS 12000

// A device's HID interfaces mount one at a time. Wait for the arrivals to stop
// before presenting ourselves to the PC, so it enumerates us once, not four
// times.
#define PROXY_SETTLE_MS 150

// Ring depth per interface. IN is device->PC (bursty: CTAPHID sends 64-byte
// continuation frames back to back), OUT is PC->device.
#define PROXY_IN_QUEUE_LEN 16
#define PROXY_OUT_QUEUE_LEN 8

#define PROXY_MAX_HID 4
static_assert(PROXY_MAX_HID <= CFG_TUD_HID,
              "raise -DCFG_TUD_HID in platformio.ini");

// Logging never touches the port directly: core 1 and the USB IRQ both log, and
// neither may block or race core 0's tud_task(). Everything goes into the ring
// and core 0 drains it in loop(). See include/log_ring.h.
LogRing g_log_ring;

static void log_emit(char const *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = '\0';
  g_log_ring.write(buf);
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
  uint8_t interval_ms;
};

// Order defines the interface order the PC sees. Keyboard first, matching the
// OnlyKey's own layout.
static proxy_itf_desc_t const k_itf[PROXY_MAX_HID] = {
    {"keyboard", desc_keyboard, sizeof(desc_keyboard), 4,
     HID_ITF_PROTOCOL_KEYBOARD, 1},
    {"fido2", desc_fido, sizeof(desc_fido), 5, HID_ITF_PROTOCOL_NONE, 1},
    {"rawhid", desc_rawhid, sizeof(desc_rawhid), 5, HID_ITF_PROTOCOL_NONE, 1},
    {"seremu", desc_seremu, sizeof(desc_seremu), 5, HID_ITF_PROTOCOL_NONE, 1},
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
struct host_link_t {
  volatile bool mounted;
  uint8_t daddr;
  uint8_t idx;
  bool has_out_ep; // interrupt OUT available, else fall back to control
};
static host_link_t g_link[PROXY_MAX_HID];

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

struct out_report_t {
  uint8_t report_id;
  uint8_t report_type;
  uint8_t len;
  uint8_t data[CFG_TUH_HID_EPOUT_BUFSIZE];
};

// device -> PC: produced by core 1, consumed by core 0.
static spsc_queue<in_report_t, PROXY_IN_QUEUE_LEN> g_inq[PROXY_MAX_HID];
// PC -> device: produced by core 0, consumed by core 1.
static spsc_queue<out_report_t, PROXY_OUT_QUEUE_LEN> g_outq[PROXY_MAX_HID];

// Each counter is written by exactly one core.
static volatile uint32_t g_in_sent, g_in_dropped;
static volatile uint32_t g_out_sent, g_out_dropped;

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
};

static volatile uint8_t g_power_cmd; // core 0 writes, core 1 consumes
static volatile bool g_power_is_on;  // core 1 writes, core 0 reads
static uint32_t g_power_off_until;   // core 1 only

// Core 1 bumps this every pass through loop1(). Core 0 watches it to tell
// "host stack running, no device found" apart from "core 1 wedged", which
// otherwise look identical from the outside.
static volatile uint32_t g_core1_ticks;
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

static void power_apply(bool on) {
  if (!on) {
    // Don't wait for the umount callbacks -- drop the PC side immediately so
    // the unplug it sees lines up with VBUS actually going away.
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      g_link[i].mounted = false;
    }
    g_link_settle_at = 0;
    pc_present_set(false);
  }
  digitalWrite(PIN_5V_EN, on ? PIN_5V_EN_STATE : !PIN_5V_EN_STATE);
  g_power_is_on = on;
}

// Non-blocking: the host stack must keep running through the off window so it
// notices the disconnect and re-enumerates on the way back up.
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
#if PROXY_ENABLE_CDC
      // Detaching to mirror the key also takes the CDC console with it -- a USB
      // device cannot drop only some of its interfaces. Without a deadline an
      // explicit "off" would remove the only way to send "on", so it expires.
      // The BOOT button still works throughout.
      g_power_off_until = millis() + PROXY_POWER_OFF_MAX_MS;
      LOGF("[pwr] VBUS off (auto-restore in %u ms; console returns with it)\r\n",
           (unsigned)PROXY_POWER_OFF_MAX_MS);
#else
      g_power_off_until = 0;
      LOGF("[pwr] VBUS off\r\n");
#endif
      break;
    case PWR_CMD_ON:
      LOGF("[pwr] VBUS on\r\n");
      g_power_off_until = 0;
      power_apply(true);
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
  LOGF("[map] unrecognised descriptor (%u bytes):\r\n\"", len);
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
  out_report_t *slot = g_outq[ITF].reserve();
  if (!slot) {
    g_out_dropped++;
    return;
  }
  if (bufsize > sizeof(slot->data)) {
    bufsize = sizeof(slot->data);
  }
  slot->report_id = report_id;
  slot->report_type = (uint8_t)report_type;
  slot->len = (uint8_t)bufsize;
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
  LOGF("core1     : %s, %lu ticks\r\n",
       g_core1_fatal ? "FATAL" : (core1_alive() ? "running" : "STALLED"),
       (unsigned long)g_core1_ticks);
  LOGF("host stack: %s, %u HID itf bound\r\n",
       g_host_started ? "started" : "NOT STARTED", g_host_hid_count);
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
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    LOGF("itf %u %-8s: %s\r\n", i, k_itf[i].name,
         g_link[i].mounted ? "linked" : "-");
  }
  LOGF("dev->PC   : %lu sent, %lu dropped\r\n", (unsigned long)g_in_sent,
       (unsigned long)g_in_dropped);
  LOGF("PC->dev   : %lu sent, %lu dropped\r\n", (unsigned long)g_out_sent,
       (unsigned long)g_out_dropped);
  LOGF("keys      : p=power-cycle 0=off 1=on i=info\r\n");
}

// When set, status repeats on its own every 3 s. Off by default so the log
// stays quiet and command output is easy to read.
static bool g_heartbeat = false;

static void print_help(void) {
  LOGF("commands: i=status  h=host pins  d=descriptor dump  p=power-cycle  "
       "0=VBUS off  1=VBUS on  t=toggle 3s heartbeat (%s)  ?=help\r\n",
       g_heartbeat ? "on" : "off");
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
    switch (PROXY_LOG.read()) {
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
    case 't':
    case 'T':
      g_heartbeat = !g_heartbeat;
      LOGF("heartbeat %s\r\n", g_heartbeat ? "on" : "off");
      break;
    case 'd':
    case 'D':
      g_dump_req = true; // core 1 performs it; sync APIs need loop1 context
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
      LOGF("[btn] power-cycle requested\r\n");
      g_power_cmd = PWR_CMD_CYCLE;
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
// is the fault light; the two never mean the same thing.
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
    .origin = -1,
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

static inline void pixel_note_activity(uint8_t itf) {
  if (itf >= PROXY_MAX_HID) {
    return;
  }
  uint32_t const now = millis();
  int8_t const cur = g_pixel_itf;
  if (cur >= 0 && cur < (int8_t)PROXY_MAX_HID &&
      (now - g_pixel_at) < PROXY_PIXEL_PULSE_MS &&
      kItfRank[itf] < kItfRank[cur]) {
    return; // don't let debug chatter paint over something more interesting
  }
  g_pixel_itf = (int8_t)itf;
  g_pixel_at = now;
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
  int8_t const itf = g_pixel_itf;
  uint32_t const since = millis() - g_pixel_at;
  if (itf >= 0 && itf < (int8_t)PROXY_MAX_HID && since < PROXY_PIXEL_PULSE_MS) {
    uint32_t const fade = 255 - (since * 255 / PROXY_PIXEL_PULSE_MS);
    uint8_t const *c = kItfColour[itf];
    r = (uint8_t)((c[0] * fade + r * (255 - fade)) / 255);
    g = (uint8_t)((c[1] * fade + g * (255 - fade)) / 255);
    b = (uint8_t)((c[2] * fade + b * (255 - fade)) / 255);
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

// The onboard LED on GPIO 13 is a fault light, nothing else: dark when idle or
// working normally, lit when something needs attention. An empty host port is
// not a fault, so no key attached means dark.
//
// Faults are:
//   - core 1 wedged, or the host stack never started
//   - a key is attached but not all four interfaces matched
//   - all four matched but the PC has not mounted the proxy
static bool led_fault(void) {
  if (g_core1_fatal || !core1_alive()) {
    return true;
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
  if (linked < PROXY_MAX_HID) {
    return true; // key present but an interface failed to match
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

static void led_service(void) {
  static uint32_t changed_at = 0;
  static bool last_fault = false;
  static bool on = false;

  bool const fault = led_fault();
  if (fault != last_fault) {
    last_fault = fault;
    changed_at = millis();
  }

  bool const want = fault && (millis() - changed_at) >= PROXY_LED_FAULT_MS;
  if (want != on) {
    on = want;
    digitalWrite(PIN_LED, on);
  }
}

void setup() {
  PROXY_LOG.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pixel_begin(); // core 0 only -- see the note above pixel_service()

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
#if !PROXY_ENABLE_CDC
  // Pure HID clone: throw the CDC configuration away and build our own.
  TinyUSBDevice.clearConfiguration();
#else
  // Keep the core's configuration and append to it. This is deliberate: the
  // CDC needs an Interface Association Descriptor for Windows to bind it in a
  // composite device, and the device-level class/subclass/protocol that go with
  // an IAD are set inside TinyUSBDevice.begin() with no public setter to
  // restore them. Building on the existing configuration inherits them.
#endif
  TinyUSBDevice.setID(PROXY_VID, PROXY_PID);
  TinyUSBDevice.setManufacturerDescriptor(PROXY_MANUFACTURER);
  TinyUSBDevice.setProductDescriptor(PROXY_PRODUCT);
  TinyUSBDevice.setSerialDescriptor(PROXY_SERIAL);

  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    g_hid[i].setReportDescriptor(k_itf[i].desc, k_itf[i].desc_len);
    g_hid[i].setBootProtocol(k_itf[i].boot_protocol);
    g_hid[i].setPollInterval(k_itf[i].interval_ms);
    // Every one of these descriptors declares an Output item, so each needs an
    // interrupt OUT endpoint -- CTAPHID and the OnlyKey app both rely on it.
    g_hid[i].enableOutEndpoint(true);
    g_hid[i].setReportCallback(nullptr, k_set_cb[i]);

    if (!g_hid[i].begin()) {
      LOGF("[dev] FAILED to add interface %u (%s)\r\n", i, k_itf[i].name);
    }
  }

  // Stay detached. Core 1 attaches us once the OnlyKey has enumerated, so the
  // PC only ever sees the proxy while a key is actually plugged in.
  LOGF("[dev] configured %04X:%04X with %u HID interfaces, waiting for key\r\n",
       PROXY_VID, PROXY_PID, PROXY_MAX_HID);
#endif // PROXY_DIAG

  print_status();
  print_help();
}

// Core 0 owns the device stack: apply whatever presence core 1 published.
static void presence_service(void) {
  bool const want = g_pc_present_req;
  if (want == g_pc_attached) {
    return;
  }

  if (want) {
    LOGF("[dev] attaching to PC\r\n");
    TinyUSBDevice.attach();
  } else {
    LOGF("[dev] detaching from PC\r\n");
    TinyUSBDevice.detach();
    for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
      g_inq[i].flush();
    }
  }
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
    LOGF("[diag] forcing re-enumeration...\r\n");
    g_power_cmd = PWR_CMD_CYCLE;
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
  console_service();
  button_service();
  led_service();
  pixel_service(); // core 0 only: the sole place the pixel PIO is touched
}

// --------------------------------------------------------------------------
// Core 1 -- PIO USB host stack
// --------------------------------------------------------------------------

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
static void forward_out(void) {
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    host_link_t const *link = &g_link[i];

    while (true) {
      out_report_t *rpt = g_outq[i].peek();
      if (!rpt) {
        break;
      }

      if (!link->mounted) {
        // Nothing to forward to -- discard rather than stall the queue.
        g_outq[i].pop();
        g_out_dropped++;
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
        break;
      }
      g_outq[i].pop();
      g_out_sent++;
      pixel_note_activity(i); // variable store only; core 0 does the rendering
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
  g_core1_ticks++; // liveness, watched by core 0
  USBHost.task();
  power_service();
  link_service();

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
  static uint32_t next_poll = 0;
  if ((int32_t)(millis() - next_poll) >= 0) {
    next_poll = millis() + 250;
    g_host_hid_count = tuh_hid_itf_get_total_count();
  }

#if !PROXY_DIAG
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
  LOGF("[host] device detached: addr %u\r\n", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc_report,
                      uint16_t desc_len) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  LOGF("[host] HID mounted: addr %u idx %u, %04X:%04X, %u-byte descriptor\r\n",
       daddr, idx, vid, pid, desc_len);

  if (!desc_report || !desc_len) {
    // Descriptors larger than CFG_TUH_ENUMERATION_BUFSIZE are not delivered.
    LOGF("[host] no report descriptor available -- cannot map interface\r\n");
    return;
  }

#if PROXY_DIAG
  // Always show the bytes in diagnostic mode, matched or not.
  dump_descriptor(desc_report, desc_len);
#endif

  int8_t const itf = match_interface(desc_report, desc_len);
  if (itf < 0) {
#if !PROXY_DIAG
    dump_descriptor(desc_report, desc_len);
#endif
    LOGF("[map] no match -- this interface will not be proxied\r\n");
    return;
  }

  tuh_itf_info_t info;
  bool const has_out_ep =
      tuh_hid_itf_get_info(daddr, idx, &info) && info.desc.bNumEndpoints >= 2;

  g_link[itf].daddr = daddr;
  g_link[itf].idx = idx;
  g_link[itf].has_out_ep = has_out_ep;
  __dmb();
  g_link[itf].mounted = true;
  g_link_settle_at = millis() + PROXY_SETTLE_MS;

  LOGF("[host] itf %u -> %s (OUT endpoint: %s)\r\n", itf, k_itf[itf].name,
       has_out_ep ? "yes" : "control only");

  if (!tuh_hid_receive_report(daddr, idx)) {
    LOGF("[host] %s: cannot arm IN endpoint\r\n", k_itf[itf].name);
  }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
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
  for (uint8_t i = 0; i < PROXY_MAX_HID; i++) {
    if (g_link[i].mounted && g_link[i].daddr == daddr && g_link[i].idx == idx) {
      in_report_t *slot = g_inq[i].reserve();
      if (!slot) {
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

  // Re-arm immediately or the interface goes silent after one report.
  tuh_hid_receive_report(daddr, idx);
}

} // extern "C"
