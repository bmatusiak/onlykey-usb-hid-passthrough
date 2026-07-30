// Non-blocking sink for TinyUSB's internal trace.
//
// Adafruit's log_printf() writes to SERIAL_TUSB_DEBUG, and on RP2040 it is
// called from tud_task(), which runs in USB IRQ context. Pointing that at a
// Serial object deadlocks the entire USB device stack the moment nobody is
// draining the port: the CDC FIFO fills, write() blocks inside the IRQ, control
// transfers stop being answered, and the host can no longer even open the port.
//
// So SERIAL_TUSB_DEBUG is pointed at this instead: an IRQ-safe ring buffer that
// drops on overflow and never blocks. loop() drains it to the real log port at
// whatever rate the host is actually consuming.
//
// Force-included via -include, so it lands in C translation units too -- keep
// everything behind __cplusplus.

#pragma once

#ifdef __cplusplus

#include <stddef.h>
#include <stdint.h>

class TusbLogSink {
public:
  // Called by log_printf on first use; the real port is opened elsewhere.
  void begin(unsigned long) {}

  // Producer: IRQ context. Drops rather than blocking when full.
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

  // Consumer: thread context. Returns -1 when empty.
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
  static const uint16_t kSize = 8192;
  volatile uint16_t head_ = 0;
  volatile uint16_t tail_ = 0;
  volatile uint32_t dropped_ = 0;
  char buf_[kSize];
};

extern TusbLogSink g_tusb_log;

#endif // __cplusplus
