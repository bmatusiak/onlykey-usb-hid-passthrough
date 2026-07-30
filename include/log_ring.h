// Non-blocking, cross-core log buffer.
//
// Two producers need this and neither can be allowed to block:
//
//  - Core 1 (the USB host stack) logs mounts, unmounts and power events. When
//    the log port is the USB CDC, writing to it from core 1 would touch the
//    device stack's FIFOs concurrently with core 0's tud_task(), which is not
//    safe across cores. Adafruit_USBD_CDC::write also spins on yield() when a
//    terminal is connected but not reading.
//  - Adafruit's log_printf() emits TinyUSB's own trace from USB IRQ context
//    when -DCFG_TUSB_DEBUG is set. Writing to a Serial object there deadlocks
//    the whole device stack the moment nothing drains the port: the CDC FIFO
//    fills, write() spins, control transfers stop being answered, and the host
//    can no longer even open the port. (Learned the hard way.)
//
// So everything is appended here instead, and core 0 drains it from loop() at
// whatever rate the port actually accepts. Overflow drops characters rather
// than making anyone wait.
//
// Force-included into C translation units when wiring up SERIAL_TUSB_DEBUG, so
// keep everything behind __cplusplus.

#pragma once

#ifdef __cplusplus

#include <stddef.h>
#include <stdint.h>

class LogRing {
public:
  // Called by log_printf on first use; the real port is opened elsewhere.
  void begin(unsigned long) {}

  // Producer: any core, including IRQ context. Drops rather than blocking.
  size_t write(const char *s) {
    size_t n = 0;
    for (; s && *s; s++) {
      uint16_t const next = (uint16_t)((head_ + 1) % kSize);
      if (next == tail_) {
        dropped_++;
        break;
      }
      buf_[head_] = *s;
      head_ = next;
      n++;
    }
    return n;
  }

  // Consumer: core 0 only. Returns -1 when empty.
  int read() {
    if (tail_ == head_) {
      return -1;
    }
    uint8_t const c = (uint8_t)buf_[tail_];
    tail_ = (uint16_t)((tail_ + 1) % kSize);
    return (int)c;
  }

  bool empty() const { return tail_ == head_; }
  uint32_t dropped() const { return dropped_; }

private:
  // Generous on purpose: with -DCFG_TUSB_DEBUG the TinyUSB trace alone runs at
  // roughly 29 KB/s, which at 4 KB evicted our own [host] lines before they
  // could be drained -- mount/unmount events went missing exactly when they
  // mattered. Prefer dropping trace noise over dropping our own messages.
  static const uint16_t kSize = 16384;
  volatile uint16_t head_ = 0;
  volatile uint16_t tail_ = 0;
  volatile uint32_t dropped_ = 0;
  char buf_[kSize];
};

extern LogRing g_log_ring;

#endif // __cplusplus
