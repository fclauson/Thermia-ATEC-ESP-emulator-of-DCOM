// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
// Modbus-RTU framing + decoding of traffic addressed to the Thermia gateway (0x0F),
// plus the heat pump's own FC23 I/O with its other bus devices (Fc23Decoder).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace modbus_sniff {

inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

inline bool crc_ok(const uint8_t *d, size_t n) {
  return n >= 4 && crc16(d, n - 2) == (uint16_t)(d[n - 2] | (d[n - 1] << 8));
}

// Splits a raw byte stream into CRC-valid frames using idle gaps. Never transmits.
class Framer {
 public:
  static constexpr size_t MAX = 260;
  explicit Framer(uint32_t gap_us = 3000, uint32_t drop_us = 30000) : gap_(gap_us), drop_(drop_us) {}

  void push(uint8_t b, uint32_t now_us) {
    if (n_ >= MAX) { n_ = 0; bad_++; }
    buf_[n_++] = b;
    last_ = now_us;
  }

  // Call regularly. Returns true when a valid frame is available (valid until next push()).
  bool poll(uint32_t now_us, const uint8_t **data, size_t *len) {
    if (n_ == 0) return false;
    uint32_t idle = now_us - last_;
    if (idle < gap_) return false;
    if (crc_ok(buf_, n_)) return emit_(0, data, len);
    // resync: we may have joined mid-frame, look for a valid frame at a later offset
    for (size_t k = 1; k + 8 <= n_; k++)
      if (crc_ok(buf_ + k, n_ - k)) { bad_++; return emit_(k, data, len); }
    if (idle >= drop_) { n_ = 0; bad_++; }  // give up on this garbage
    return false;
  }

  uint32_t ok() const { return ok_; }
  uint32_t bad() const { return bad_; }
  // Microseconds since the last byte was seen on the bus. Used to check the bus is
  // quiet before transmitting anything.
  uint32_t idle_us(uint32_t now_us) const { return now_us - last_; }

 private:
  bool emit_(size_t off, const uint8_t **data, size_t *len) {
    *data = buf_ + off;
    *len = n_ - off;
    n_ = 0;
    ok_++;
    return true;
  }
  uint8_t buf_[MAX];
  size_t n_{0};
  uint32_t last_{0}, gap_, drop_;
  uint32_t ok_{0}, bad_{0};
};

// Decodes frames for one slave address. Calls cb(register, value) for every register seen.
class Decoder {
 public:
  explicit Decoder(uint8_t addr) : addr_(addr) {}
  uint8_t addr() const { return addr_; }

  template <typename F>
  void handle(const uint8_t *d, size_t n, F &&cb) {
    if (n < 8 || d[0] != addr_) return;
    uint8_t fc = d[1];
    if (fc == 0x10) {  // master writes registers to gateway (ack is 8 bytes, ignored)
      uint16_t start = rd_(d + 2), qty = rd_(d + 4);
      uint8_t bc = d[6];
      if (n == (size_t)bc + 9 && bc == qty * 2 && qty > 0)
        for (uint16_t i = 0; i < qty; i++) cb((uint16_t)(start + i), rd_(d + 7 + 2 * i));
    } else if (fc == 0x03) {
      if (n == 8) {  // read request: remember what was asked
        rq_start_ = rd_(d + 2);
        rq_qty_ = rd_(d + 4);
      } else if (rq_qty_ && d[2] == rq_qty_ * 2 && n == (size_t)d[2] + 5) {  // response
        for (uint16_t i = 0; i < rq_qty_; i++) cb((uint16_t)(rq_start_ + i), rd_(d + 3 + 2 * i));
        rq_qty_ = 0;
      }
    }
    // FC 0x17 (handshake) is deliberately ignored for now.
  }

 private:
  static uint16_t rd_(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
  uint8_t addr_;
  uint16_t rq_start_{0}, rq_qty_{0};
};

// Decodes FC23 (0x17, read/write multiple registers) traffic between the heat pump and
// its OTHER bus devices (0x02, 0x04, ...). This is the heat pump's own real-time I/O,
// sent about once a second, so it is far fresher than the gateway's copy (2000..2239,
// re-sent only every ~21 s) and it keeps flowing even when no gateway is connected.
//   on_cmd(dev, addr, value)   -- every value the heat pump WRITES to dev (its commands)
//   on_reply(dev, addr, value) -- every value dev returns for the read part of the request
// Addresses are the device's own Modbus addresses (e.g. 0xA80C), not gateway numbers.
// The caller must not pass gateway (0x0F) frames: those are the handshake.
class Fc23Decoder {
 public:
  template <typename C, typename R>
  void handle(const uint8_t *d, size_t n, C &&on_cmd, R &&on_reply) {
    if (n < 5 || d[1] != 0x17) return;
    const uint8_t dev = d[0];
    // Request from the heat pump: addr fc rs(2) rq(2) ws(2) wq(2) bc data crc(2)
    if (n >= 13 && n == (size_t) d[10] + 13) {
      uint16_t rs = rd_(d + 2), rq = rd_(d + 4), ws = rd_(d + 6), wq = rd_(d + 8);
      if (wq > 0 && d[10] == wq * 2 && rq > 0 && rq <= 125) {
        for (uint16_t i = 0; i < wq; i++) on_cmd(dev, (uint16_t) (ws + i), rd_(d + 11 + 2 * i));
        pend_dev_ = dev;
        pend_start_ = rs;
        pend_qty_ = rq;
        requests_++;
        return;
      }
    }
    // Reply from the device: addr fc bc data crc(2), must match the pending request
    if (pend_qty_ && dev == pend_dev_ && n == (size_t) d[2] + 5 && d[2] == pend_qty_ * 2) {
      for (uint16_t i = 0; i < pend_qty_; i++) on_reply(dev, (uint16_t) (pend_start_ + i), rd_(d + 3 + 2 * i));
      pend_qty_ = 0;
      replies_++;
    }
  }
  uint32_t requests() const { return requests_; }
  uint32_t replies() const { return replies_; }

 private:
  static uint16_t rd_(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
  uint8_t pend_dev_{0};
  uint16_t pend_start_{0}, pend_qty_{0};
  uint32_t requests_{0}, replies_{0};
};

}  // namespace modbus_sniff
