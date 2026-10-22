#pragma once
// bus_events.h  v2026.10.22.1  event ring, watchlist and settings-push helpers (logging only)
//
// Pure C++ (no ESPHome), host-tested. Nothing here affects what the ESP transmits.
//
//  - EventRing: the last EVENT_CAPACITY changes of watched registers / live values, each with a
//    sequence number and the ESP uptime (ms) at which it happened. Served in /registers.json so
//    the 5-minute collector can write exact-time "event" rows for short events (defrosts,
//    test-mode steps) that snapshots miss.
//  - Watchlist: which registers and live keys produce events.
//  - settings_block_bit(): the mailbox bit that selects a settings block (all 16 W1 bits and
//    W0 bit 0 confirmed on our bus; the other W0 bits follow the CM930 map).
//  - RequestMemory: when each mailbox bit was last served set, so an FC16 settings push from
//    the heat pump can be classed as "requested" (its upload bit was served recently) or
//    "UNSOLICITED".

#include <cstdint>
#include <cstdio>
#include <string>

namespace thermia {

// ---------------------------------------------------------------- watchlist
inline bool is_watched_reg(uint16_t r) {
  switch (r) {
    case 1363: case 2013: case 2018: case 2032: case 2083: case 2091:
    case 2132: case 2148: case 2149: case 2150: case 2171: case 2172:
      return true;
    default:
      return r >= 1780 && r <= 1798;  // test-mode block 0x06F4
  }
}

// Live keys: heat pump commands to dev4 ABF4 (ODU run mode), dev2 A80C (outputs), dev2 A811.
inline bool is_watched_live(bool reply, uint8_t dev, uint16_t addr) {
  if (reply) return false;
  return (dev == 4 && addr == 0xABF4) || (dev == 2 && (addr == 0xA80C || addr == 0xA811));
}

// ---------------------------------------------------------------- event ring
struct BusEvent {
  uint32_t seq;    // 1, 2, 3, ... since boot (0 = empty slot)
  uint32_t ms;     // ESP uptime in ms when it happened
  uint16_t addr;   // register number, or live address
  uint16_t value;  // raw unsigned 16-bit
  uint8_t dev;     // 0 = gateway register, otherwise live device address
  uint8_t reply;   // live only: 0 = heat pump wrote, 1 = device replied
};

// Key text, identical to the /registers.json keys: "2149" or "dev04.w.44020".
inline std::string event_key(const BusEvent &e) {
  char b[24];
  if (e.dev == 0)
    snprintf(b, sizeof(b), "%u", (unsigned) e.addr);
  else
    snprintf(b, sizeof(b), "dev%02X.%c.%u", (unsigned) e.dev, e.reply ? 'r' : 'w', (unsigned) e.addr);
  return b;
}

class EventRing {
 public:
  static constexpr uint16_t EVENT_CAPACITY = 256;

  void push(uint32_t ms, uint8_t dev, bool reply, uint16_t addr, uint16_t value) {
    BusEvent &e = buf_[head_];
    e.seq = ++seq_;
    e.ms = ms;
    e.dev = dev;
    e.reply = reply ? 1 : 0;
    e.addr = addr;
    e.value = value;
    head_ = (uint16_t) ((head_ + 1) % EVENT_CAPACITY);
    if (count_ < EVENT_CAPACITY) count_++;
  }
  uint32_t last_seq() const { return seq_; }
  uint16_t count() const { return count_; }
  // i = 0 is the oldest event still held.
  const BusEvent &at(uint16_t i) const {
    const uint16_t oldest = (uint16_t) ((head_ + EVENT_CAPACITY - count_) % EVENT_CAPACITY);
    return buf_[(oldest + i) % EVENT_CAPACITY];
  }

 private:
  BusEvent buf_[EVENT_CAPACITY]{};
  uint16_t head_{0}, count_{0};
  uint32_t seq_{0};
};

// ---------------------------------------------------------------- settings blocks
// word: 0 = upper settings (W0 re-read / W2 upload request), 1 = lower (W1 / W3).
inline bool settings_block_bit(uint16_t start, uint8_t &word, uint8_t &bit) {
  static const uint16_t LOWER[16] = {1000, 1020, 1040, 1070, 1090, 1110, 1130, 1150,
                                     1170, 1190, 1210, 1240, 1270, 1290, 1310, 1330};
  for (uint8_t b = 0; b < 16; b++)
    if (LOWER[b] == start) { word = 1; bit = b; return true; }
  if (start == 1350) { word = 0; bit = 0; return true; }
  if (start == 1370) { word = 0; bit = 1; return true; }
  if (start >= 1403 && start <= 1733 && (start - 1403) % 33 == 0) {
    word = 0; bit = (uint8_t) (2 + (start - 1403) / 33); return true;
  }
  if (start == 1770) { word = 0; bit = 13; return true; }
  if (start == 1777) { word = 0; bit = 14; return true; }
  if (start == 1780) { word = 0; bit = 15; return true; }
  return false;
}

// Mailbox steady state served between events: W0..W3 = 0, W4 = 0x077F, W5 = 6.
inline bool mailbox_is_steady(const uint16_t w[6]) {
  return w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0x077F && w[5] == 6;
}

// When each bit of W0..W3 was last SERVED set (ESP uptime ms), for push classification.
class RequestMemory {
 public:
  static constexpr uint32_t WINDOW_MS = 60000;  // a push within 60 s of the request counts

  void note_served(const uint16_t w[6], uint32_t now_ms) {
    for (uint8_t i = 0; i < 4; i++)
      for (uint8_t b = 0; b < 16; b++)
        if (w[i] & (1u << b)) { t_[i][b] = now_ms; set_[i][b] = true; }
  }
  // Was the bit for this block served set recently? upload: W2/W3 (upload request);
  // reread: W0/W1 (re-read flag). Returns a short description, or nullptr if neither.
  const char *why(uint8_t word, uint8_t bit, uint32_t now_ms) const {
    const uint8_t up = word == 1 ? 3 : 2, rr = word == 1 ? 1 : 0;
    if (recent_(up, bit, now_ms)) return word == 1 ? "requested (1803 upload bit)" : "requested (1802 upload bit)";
    if (recent_(rr, bit, now_ms)) return word == 1 ? "requested (1801 re-read bit)" : "requested (1800 re-read bit)";
    return nullptr;
  }

 private:
  bool recent_(uint8_t i, uint8_t b, uint32_t now_ms) const {
    return set_[i][b] && (now_ms - t_[i][b]) <= WINDOW_MS;
  }
  uint32_t t_[4][16]{};
  bool set_[4][16]{};
};

}  // namespace thermia
