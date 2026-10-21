// gateway_responder.h  v2026.10.20.5  gateway reply builder, two-pulse writes, resync
// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
//
// The ESP answers the heat pump AS the gateway. This header only BUILDS the exact reply
// bytes and holds the store and mailbox state; it never touches the bus. The ESPHome
// component decides whether it is armed, whether the bus is idle and whether a real gateway
// is absent, and only then sends what build_reply() returns.
//
// Reply model (all byte formats and the ~14 ms reply timing verified against captures):
//   * FC23 handshake : 0F 17 10 <16-byte XXTEA(challenge)> CRC          (cm930_handshake.h)
//   * FC16 write     : 8-byte ack = request[0..5] (addr fc start qty) + CRC
//   * FC03 read      : 0F 03 <bytecount> <data> CRC, data served from the store
//
// "Only valid data" rule (user requirement, 18 Oct 2026): the responder serves a register
// value ONLY if it has been seen on the real bus THIS SESSION. The mailbox (1800..1805) is
// the sole block the heat pump actually reads back, and the responder AUTHORS it (quiescent
// 0,0,0,0,1919,6; 1801/1800 set only when the responder itself wants a re-read), so it is
// always available at boot without seeding. For any OTHER read, if any requested register
// has not been seen this session, build_reply() returns NO reply (stay silent) rather than
// invent a value. The only values that ever differ from what the heat pump last wrote are
// the single registers staged by stage_write() (one per user write).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "modbus_rtu.h"
#include "cm930_handshake.h"

namespace cm930 {

class GatewayResponder {
 public:
  enum class Kind : uint8_t { NONE, HANDSHAKE, ACK, READ, SUPPRESSED_UNSEEN, SUPPRESSED_BADREQ };

  struct Reply {
    Kind kind{Kind::NONE};
    uint8_t bytes[260]{};
    size_t len{0};
    uint16_t start{0}, qty{0};
    bool was_mailbox{false};
    bool has_reply() const { return len > 0; }
  };

  static constexpr uint16_t MAILBOX_START = 1800;
  static constexpr uint16_t MAILBOX_QTY = 6;
  static constexpr uint16_t MAILBOX_QUIESCENT[6] = {0, 0, 0, 0, 1919, 6};
  // Full-table resync pattern (confirmed 19 Oct 2026, full-resync-trigger.md): serving the
  // mailbox as this makes the heat pump re-push its whole 1000-1798 settings table without a
  // reboot or handshake. Revert to quiescent when done. Index order 1800..1805.
  static constexpr uint16_t MAILBOX_RESYNC[6] = {0, 0, 32767, 65535, 128, 7};

  explicit GatewayResponder(uint8_t gw_addr = 0x0F) : addr_(gw_addr) { reset_mailbox_(); }
  void set_addr(uint8_t a) { addr_ = a; }  // never construct a temporary (12 KB), see gateway_shadow.h

  // --- store: seeded ONLY by what the heat pump writes this session ---
  // Call for EVERY frame so the store tracks the real bus, armed or not.
  // Returns true if the frame was a settings/live write that updated the store.
  bool observe(const uint8_t *d, size_t n) {
    if (n < 11 || d[0] != addr_ || d[1] != 0x10) return false;
    const uint16_t start = rd_(d + 2), qty = rd_(d + 4);
    const uint8_t bc = d[6];
    if (bc != qty * 2 || n != (size_t) bc + 9 || qty == 0) return false;
    for (uint16_t i = 0; i < qty; i++) {
      const uint16_t reg = start + i;
      if (reg < STORE) {
        store_[reg] = rd_(d + 7 + 2 * i);
        seen_[reg] = true;
      }
    }
    if (start <= 1798 && start + qty > 1000) full_table_regs_seen_ += qty;  // rough boot-table progress
    return true;
  }

  // Mark that a complete boot-table push (1000..1798) has been observed this session. The
  // component calls this when it sees the table push finish; until then, ready() is false
  // and the component must NOT arm into takeover.
  void note_table_complete() { table_complete_ = true; }
  bool table_complete() const { return table_complete_; }
  bool has(uint16_t reg) const { return reg < STORE && seen_[reg]; }
  uint16_t get(uint16_t reg) const { return reg < STORE ? store_[reg] : 0; }

  // Build the reply the gateway would send for this request frame. Does NOT transmit.
  // Returns Kind::NONE with len==0 when there is nothing to answer (not addressed to us,
  // not a request, or a read we must suppress under the only-valid-data rule).
  Reply build_reply(const uint8_t *d, size_t n) {
    Reply r;
    if (n < 4 || d[0] != addr_) return r;
    const uint8_t fc = d[1];
    if (fc == 0x17) return build_handshake_(d, n);
    if (fc == 0x10) return build_ack_(d, n);
    if (fc == 0x03) return build_read_(d, n);
    return r;  // any other function code: we do not answer
  }

  // True if frame d is an FC03 read addressed to `addr` of a SETTINGS BLOCK (i.e. a real
  // register block, not the 1800x6 mailbox poll). On success fills start/qty. Used to detect
  // the heat pump re-reading a block after we raised a write flag (adoption confirmation).
  static bool is_settings_block_read(const uint8_t *d, size_t n, uint8_t addr,
                                     uint16_t &start, uint16_t &qty) {
    if (n < 8 || d[0] != addr || d[1] != 0x03) return false;
    start = (uint16_t) ((d[2] << 8) | d[3]);
    qty = (uint16_t) ((d[4] << 8) | d[5]);
    if (qty == 0) return false;
    if (start == MAILBOX_START && qty == MAILBOX_QTY) return false;  // the mailbox, not a block
    return true;
  }

  // --- mailbox control ---
  void set_mailbox(uint16_t idx, uint16_t value) {
    if (idx < MAILBOX_QTY) store_[MAILBOX_START + idx] = value;
  }
  uint16_t mailbox(uint16_t idx) const { return idx < MAILBOX_QTY ? store_[MAILBOX_START + idx] : 0; }

  // --- Stage a single register change, delivered via a mailbox re-read flag ---
  // Changes exactly one register in the store (clamped by the caller) and raises the given
  // mailbox flag (idx=1 -> 1801, value=flag) for the heat pump to re-read that block. The
  // change is refused unless the register has been SEEN this session, i.e. we hold the real
  // surrounding block so only this one value differs from what the heat pump last wrote.
  // Returns false (and does nothing) if the register was never seen.
  bool stage_write(uint16_t reg, uint16_t value, uint16_t flag_idx, uint16_t flag_value,
                   uint32_t now_ms = 0, uint32_t max_hold_ms = 15000) {
    if (!(reg < STORE && seen_[reg])) return false;      // never invent surrounding values
    store_[reg] = value;
    seen_[reg] = true;
    if (flag_idx < MAILBOX_QTY) store_[MAILBOX_START + flag_idx] = flag_value;
    pending_flag_idx_ = flag_idx;
    pending_flag_value_ = flag_value;     // remember the value so we can raise it a second time
    pending_flag_polls_ = 1;              // serve this pulse on one mailbox read, then clear
    // The REAL gateway raises the flag TWICE per change (proven on our bus: a single online
    // change produced 1801=1 at t=11.4 and again at t=24.0, ~2 quiescent polls apart, with the
    // HP re-reading the block after EACH pulse). A single pulse applies the value the first time
    // but the HP treats the second pulse as the commit/confirm, so one pulse "works once but not
    // subsequently". We replicate the two-pulse pattern: pulse, gap, pulse.
    pending_flag_repeats_ = 1;            // one more pulse after this one
    pending_flag_gap_polls_ = 0;          // (set when the first pulse clears)
    flag_deadline_ms_ = now_ms + max_hold_ms;  // whole sequence must finish by here (starvation guard)
    flag_has_deadline_ = (now_ms != 0);
    return true;
  }
  // Called by the component right after it has SENT a mailbox reply, so the flag is raised
  // for exactly one poll (matching the real gateway's one-poll pulse).
  void after_mailbox_sent() {
    // Phase 1: a pulse is being served. Count it down; when it ends, clear the flag.
    if (pending_flag_polls_ > 0) {
      if (--pending_flag_polls_ == 0 && pending_flag_idx_ < MAILBOX_QTY) {
        store_[MAILBOX_START + pending_flag_idx_] = 0;  // flag served for its one poll -> clear
        if (pending_flag_repeats_ > 0) {
          pending_flag_gap_polls_ = 2;   // ~2 quiescent polls before the next pulse (matches real gw)
        } else {
          flag_has_deadline_ = false;    // sequence complete
        }
      }
      return;
    }
    // Phase 2: quiescent gap between pulses. Count it down; then raise the flag again.
    if (pending_flag_gap_polls_ > 0) {
      if (--pending_flag_gap_polls_ == 0 && pending_flag_repeats_ > 0) {
        pending_flag_repeats_--;
        if (pending_flag_idx_ < MAILBOX_QTY)
          store_[MAILBOX_START + pending_flag_idx_] = pending_flag_value_;  // second pulse
        pending_flag_polls_ = 1;
      }
    }
  }
  bool flag_pending() const {
    return pending_flag_polls_ > 0 || pending_flag_gap_polls_ > 0 || pending_flag_repeats_ > 0;
  }

  // Request a full-table resync: serve the extreme-value mailbox pattern for up to
  // `polls` mailbox reads (the real gateway held it ~34 s, i.e. ~8 polls at 4.2 s). The
  // responder reverts to quiescent automatically once the hold expires or cancel_resync()
  // is called. While active, resync_active() is true.
  // Start a resync hold that lasts `duration_ms` of WALL-CLOCK time. Time-based, NOT tied to
  // serving mailbox reads: the re-push the resync triggers keeps the bus busy and can starve
  // our mailbox replies, so a send-count countdown could never expire (it would loop forever).
  // Caller supplies now_ms; call tick(now_ms) regularly to let it expire.
  void request_resync(uint32_t now_ms, uint32_t duration_ms = 35000) {
    for (uint16_t i = 0; i < MAILBOX_QTY; i++) store_[MAILBOX_START + i] = MAILBOX_RESYNC[i];
    resync_until_ms_ = now_ms + duration_ms;
    resync_active_ = true;
  }
  void cancel_resync() {
    if (resync_active_) { resync_active_ = false; restore_quiescent_mailbox_(); }
  }
  // Expire the resync hold by wall clock. Call every component loop.
  void tick(uint32_t now_ms) {
    if (resync_active_ && (int32_t) (now_ms - resync_until_ms_) >= 0) {
      resync_active_ = false;
      restore_quiescent_mailbox_();
    }
    if (flag_has_deadline_ && (int32_t) (now_ms - flag_deadline_ms_) >= 0) {
      flag_has_deadline_ = false;
      pending_flag_polls_ = 0;
      pending_flag_gap_polls_ = 0;
      pending_flag_repeats_ = 0;
      if (pending_flag_idx_ < MAILBOX_QTY) store_[MAILBOX_START + pending_flag_idx_] = 0;
    }
  }
  bool resync_active() const { return resync_active_; }

  uint32_t handshakes_built() const { return hs_built_; }
  uint32_t acks_built() const { return acks_built_; }
  uint32_t reads_built() const { return reads_built_; }
  uint32_t reads_suppressed() const { return reads_suppressed_; }

 private:
  static uint16_t rd_(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
  static constexpr uint16_t STORE = 4096;

  void restore_quiescent_mailbox_() {
    for (uint16_t i = 0; i < MAILBOX_QTY; i++) store_[MAILBOX_START + i] = MAILBOX_QUIESCENT[i];
  }
  void reset_mailbox_() {
    for (uint16_t i = 0; i < MAILBOX_QTY; i++) {
      store_[MAILBOX_START + i] = MAILBOX_QUIESCENT[i];
      seen_[MAILBOX_START + i] = true;  // responder-authored: always available
    }
  }

  static void put_crc_(Reply &r) {
    const uint16_t c = modbus_rtu::crc16(r.bytes, r.len);
    r.bytes[r.len++] = (uint8_t) (c & 0xFF);
    r.bytes[r.len++] = (uint8_t) (c >> 8);
  }

  Reply build_handshake_(const uint8_t *d, size_t n) {
    Reply r;
    // request: addr fc rs(2) rq(2) ws(2) wq(2) bc=16 challenge(16) crc
    if (n < 13 || n != (size_t) d[10] + 13) return r;
    const uint16_t rq = rd_(d + 2), rqty = rd_(d + 4), wqty = rd_(d + 8);
    if (rqty != 8 || wqty != 8 || d[10] != 16) return r;  // not the canonical handshake
    (void) rq;
    uint8_t ans[16];
    handshake_response(d + 11, ans);
    r.bytes[r.len++] = addr_;
    r.bytes[r.len++] = 0x17;
    r.bytes[r.len++] = 16;
    memcpy(r.bytes + r.len, ans, 16);
    r.len += 16;
    put_crc_(r);
    r.kind = Kind::HANDSHAKE;
    hs_built_++;
    return r;
  }

  Reply build_ack_(const uint8_t *d, size_t n) {
    Reply r;
    // Only acknowledge a well-formed write request (has data); never "ack" an 8-byte frame
    // (that IS an ack, i.e. a gateway reply, not a request).
    const uint16_t qty = rd_(d + 4);
    const uint8_t bc = (n >= 7) ? d[6] : 0;
    if (n < 11 || bc != qty * 2 || n != (size_t) bc + 9 || qty == 0) {
      r.kind = Kind::SUPPRESSED_BADREQ;
      return r;
    }
    // ack = addr fc start qty + CRC  (= request[0..5])
    memcpy(r.bytes, d, 6);
    r.len = 6;
    put_crc_(r);
    r.kind = Kind::ACK;
    r.start = rd_(d + 2);
    r.qty = qty;
    acks_built_++;
    return r;
  }

  Reply build_read_(const uint8_t *d, size_t n) {
    Reply r;
    if (n != 8) return r;  // a read request is exactly 8 bytes
    const uint16_t start = rd_(d + 2), qty = rd_(d + 4);
    r.start = start;
    r.qty = qty;
    if (qty == 0 || qty > 125) {
      r.kind = Kind::SUPPRESSED_BADREQ;
      return r;
    }
    // Only-valid-data rule: every requested register must have been seen this session.
    for (uint16_t i = 0; i < qty; i++) {
      const uint16_t reg = start + i;
      if (!(reg < STORE && seen_[reg])) {
        r.kind = Kind::SUPPRESSED_UNSEEN;
        reads_suppressed_++;
        return r;  // stay silent rather than invent a value
      }
    }
    r.bytes[r.len++] = addr_;
    r.bytes[r.len++] = 0x03;
    r.bytes[r.len++] = (uint8_t) (qty * 2);
    for (uint16_t i = 0; i < qty; i++) {
      const uint16_t v = store_[start + i];
      r.bytes[r.len++] = (uint8_t) (v >> 8);
      r.bytes[r.len++] = (uint8_t) (v & 0xFF);
    }
    put_crc_(r);
    r.kind = Kind::READ;
    reads_built_++;
    r.was_mailbox = (start == MAILBOX_START && qty == MAILBOX_QTY);
    return r;
  }

  uint8_t addr_;
  uint16_t store_[STORE]{};
  bool seen_[STORE]{};
  bool table_complete_{false};
  uint32_t full_table_regs_seen_{0};
  uint32_t hs_built_{0}, acks_built_{0}, reads_built_{0}, reads_suppressed_{0};
  uint16_t pending_flag_idx_{0};
  uint16_t pending_flag_value_{0};
  uint16_t pending_flag_polls_{0};
  uint16_t pending_flag_gap_polls_{0};
  uint16_t pending_flag_repeats_{0};
  bool resync_active_{false};
  uint32_t resync_until_ms_{0};
  bool flag_has_deadline_{false};
  uint32_t flag_deadline_ms_{0};
};

}  // namespace cm930
