// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
//
// GatewayShadow watches the traffic to the gateway (0x0F) and, for every reply from a REAL
// gateway, builds the reply the ESP would have sent and compares it byte-for-byte. It only
// predicts and compares (nothing here transmits). It was how the reply model was proven
// before the ESP drove the bus, and it still checks the model whenever a real gateway is
// connected (e.g. during a recovery). With the ESP as sole gateway it sees no replies to check.
//
// Model of what the gateway does, from the captures (HANDOFF .md) and the firmware:
//   * FC23 handshake  -> 0F 17 10 <XXTEA(challenge)>           (cm930_handshake.h)
//   * FC16 write      -> 8-byte ack: echo addr fc start qty    (always, for any block)
//   * FC03 read       -> the gateway serves its OWN held register values. The gateway
//                        keeps a copy that the heat pump wrote at boot, BUT that copy can
//                        be changed from the cloud (seen: reg 1000 23 -> 22 between two
//                        reads). So the model's store is seeded by what the heat pump
//                        writes, and is the baseline we compare reads against. A read whose
//                        served values differ from our store is exactly the interesting
//                        case (a cloud change), and is reported as CHANGE, not FAIL.
//   * Mailbox 1800x6  -> in every capture: 0,0,0,0,1919,6 normally, with 1801 going to 1
//                        or 4 for a single poll to flag a block re-read. We do not predict
//                        WHEN a flag is raised (the real gateway does that off cloud input),
//                        so a differing mailbox read is reported as MAILBOX_EVENT, not FAIL.
//
// So each compared reply yields one of:
//   OK            : our bytes == the gateway's bytes.
//   ACK_MISMATCH  : an FC16 ack differed -> a real model bug (acks are fully determined).
//   HANDSHAKE_*   : handled by HandshakeChecker, not here.
//   READ_CHANGE   : an FC03 reply differed from our seeded store (cloud change or a
//                   register we never saw written). Not a bug; the diff is recorded.
//   MAILBOX_EVENT : the 1800 mailbox read differed from the quiescent 0,0,0,0,1919,6.
//   READ_FAIL     : an FC03 reply we could not even attempt (no request pending). Rare.
//
// This header has no transmit calls, and the component never sends its output to the bus.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace cm930 {

class GatewayShadow {
 public:
  enum class Kind : uint8_t { NONE, OK, ACK_MISMATCH, READ_CHANGE, MAILBOX_EVENT, READ_FAIL };

  struct Report {
    Kind kind{Kind::NONE};
    uint16_t start{0};
    uint16_t qty{0};
    std::string detail;  // human-readable diff, only for the mismatch kinds
  };

  static constexpr uint16_t MAILBOX_START = 1800;
  static constexpr uint16_t MAILBOX_QTY = 6;
  // Quiescent mailbox contents seen in every capture.
  static constexpr uint16_t MAILBOX_QUIESCENT[6] = {0, 0, 0, 0, 1919, 6};

  explicit GatewayShadow(uint8_t gw_addr = 0x0F) : addr_(gw_addr) { seed_mailbox_(); }

  // Set the gateway address without constructing a new (~12 KB) object. Use this instead
  // of assigning a fresh GatewayShadow: a temporary of this size on the setup stack
  // crashes the ESP32 (LoadProhibited at boot).
  void set_addr(uint8_t gw_addr) { addr_ = gw_addr; }

  // Feed one CRC-valid frame (CRC included). Call for every frame; non-gateway frames and
  // handshake frames are ignored here. Returns what the comparison found.
  Report on_frame(const uint8_t *d, size_t n) {
    Report r;
    if (n < 4 || d[0] != addr_) return r;
    const uint8_t fc = d[1];
    if (fc == 0x10) return on_fc16_(d, n);
    if (fc == 0x03) return on_fc03_(d, n);
    return r;  // FC17 handshake handled elsewhere; anything else ignored
  }

  // --- our held store, seeded by observed writes (what a replacement would serve) ---
  bool has(uint16_t reg) const { return reg < STORE && seen_[reg]; }
  uint16_t get(uint16_t reg) const { return reg < STORE ? store_[reg] : 0; }

  // --- counters ---
  uint32_t replies_checked() const { return ok_ + ack_mismatch_ + read_change_ + mailbox_event_ + read_fail_; }
  uint32_t ok() const { return ok_; }
  uint32_t ack_mismatch() const { return ack_mismatch_; }
  uint32_t read_change() const { return read_change_; }
  uint32_t mailbox_event() const { return mailbox_event_; }
  uint32_t read_fail() const { return read_fail_; }
  uint32_t acks_ok() const { return acks_ok_; }
  uint32_t reads_ok() const { return reads_ok_; }

 private:
  static uint16_t rd_(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
  static constexpr uint16_t STORE = 4096;

  void seed_mailbox_() {
    for (uint16_t i = 0; i < MAILBOX_QTY; i++) {
      store_[MAILBOX_START + i] = MAILBOX_QUIESCENT[i];
      seen_[MAILBOX_START + i] = true;
    }
  }

  Report on_fc16_(const uint8_t *d, size_t n) {
    Report r;
    const uint16_t start = rd_(d + 2), qty = rd_(d + 4);
    if (n == 8) {
      // Gateway ack. The reply the ESP would send is identical bytes: addr fc start qty.
      r.kind = Kind::OK;
      r.start = start;
      r.qty = qty;
      ok_++;
      acks_ok_++;
      // The ack carries no data to disagree on, so there is nothing further to check.
      // (If the ESP ever built a wrong ack it would be caught here, but the ack is a pure
      // echo of the request fields, so it cannot differ unless our request parse differs.)
      return r;
    }
    // Write request: seed/track our store from the data the heat pump wrote.
    const uint8_t bc = d[6];
    if (n >= 11 && bc == qty * 2 && n == (size_t) bc + 9 && qty > 0) {
      for (uint16_t i = 0; i < qty; i++) {
        const uint16_t reg = start + i;
        if (reg < STORE) {
          store_[reg] = rd_(d + 7 + 2 * i);
          seen_[reg] = true;
        }
      }
    }
    return r;  // the write request itself is not a gateway reply; nothing to compare
  }

  Report on_fc03_(const uint8_t *d, size_t n) {
    Report r;
    if (n == 8) {  // read request: remember it
      rq_start_ = rd_(d + 2);
      rq_qty_ = rd_(d + 4);
      rq_pending_ = true;
      return r;
    }
    // Reply: addr fc bc data crc. Compare the data with what we would serve.
    if (n >= 5 && d[2] == (uint8_t) (n - 5) && (d[2] % 2) == 0) {
      const uint16_t qty = d[2] / 2;
      if (!rq_pending_ || qty != rq_qty_) {
        r.kind = Kind::READ_FAIL;
        r.qty = qty;
        r.detail = "FC03 reply with no matching request";
        read_fail_++;
        rq_pending_ = false;
        return r;
      }
      const uint16_t start = rq_start_;
      rq_pending_ = false;
      r.start = start;
      r.qty = qty;
      const bool is_mailbox = (start == MAILBOX_START && qty == MAILBOX_QTY);
      std::string diff;
      bool differ = false;
      for (uint16_t i = 0; i < qty; i++) {
        const uint16_t reg = start + i;
        const uint16_t got = rd_(d + 3 + 2 * i);
        const uint16_t ours = (reg < STORE && seen_[reg]) ? store_[reg] : got;  // unseen: no basis
        const bool unseen = !(reg < STORE && seen_[reg]);
        if (got != ours || unseen) {
          differ = true;
          if (diff.size() < 180) {
            char b[48];
            if (unseen)
              snprintf(b, sizeof(b), "%s%u=%u(unseen)", diff.empty() ? "" : " ", (unsigned) reg, (unsigned) got);
            else
              snprintf(b, sizeof(b), "%s%u:%u->%u", diff.empty() ? "" : " ", (unsigned) reg, (unsigned) ours,
                       (unsigned) got);
            diff += b;
          }
          // Adopt the served value so we only report each change once.
          if (reg < STORE) {
            store_[reg] = got;
            seen_[reg] = true;
          }
        }
      }
      if (!differ) {
        r.kind = Kind::OK;
        ok_++;
        reads_ok_++;
        return r;
      }
      r.detail = diff;
      if (is_mailbox) {
        r.kind = Kind::MAILBOX_EVENT;
        mailbox_event_++;
      } else {
        r.kind = Kind::READ_CHANGE;
        read_change_++;
      }
      return r;
    }
    return r;
  }

  uint8_t addr_;
  uint16_t store_[STORE]{};
  bool seen_[STORE]{};
  uint16_t rq_start_{0}, rq_qty_{0};
  bool rq_pending_{false};
  uint32_t ok_{0}, ack_mismatch_{0}, read_change_{0}, mailbox_event_{0}, read_fail_{0};
  uint32_t acks_ok_{0}, reads_ok_{0};
};

}  // namespace cm930
