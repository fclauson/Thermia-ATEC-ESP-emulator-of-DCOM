// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
//
// Holds the last value seen on the bus for every register (0..REG_MAX-1), and nothing
// else: no flash, no defaults, no guesses. A register that has not been seen since boot
// simply has no value (has() == false), and the ESPHome side never publishes it, so Home
// Assistant shows it as "unknown" until a real frame carries it.
//
// Also implements the two log buttons:
//   CHANGES     "Log Changed Registers Since Last Logged": every register whose value
//               differs from what it was at the previous press. The first press after boot
//               only records a baseline. Registers first seen since the previous press are
//               counted in the summary line, not listed (they have no "before" value).
//   NEW_UNMAPPED "Dump Unmapped Registers": registers with no sensor in the YAML that have
//               appeared since the previous press (on the first press: since boot), with
//               their current value. The point is to spot registers worth adding to YAML.
// Each button keeps its own memory; pressing one never affects the other.
#pragma once
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "reg_ranges.h"

namespace thermia {

// Turn a raw 16-bit bus word into the number shown to the user. Signed registers are
// two's complement on the wire (e.g. 1077 outdoor stop: 0xFFEC = -20). Registers whose
// negative range is stored as a positive MAGNITUDE (1070, 1111) are NOT handled here:
// they are configured signed and corrected with a YAML filter (multiply: -1 / -10).
inline float decode_value(uint16_t raw, bool is_signed) {
  return is_signed ? (float) (int16_t) raw : (float) raw;
}

// How a register with a negative range is stored on the bus. Only use an encoding that has
// been seen on the real bus for that register (decode what the heat pump sends first).
//   TWOS_COMPLEMENT: -20 is sent as 0xFFEC (seen: 1077 outdoor stop)
//   NEG_MAGNITUDE:   -60 is sent as 60      (seen: 1070 integral A1)
enum class Encoding : uint8_t { TWOS_COMPLEMENT, NEG_MAGNITUDE };

// Turn a display value (already clamped to the register's range) into the bus word.
inline uint16_t encode_value(int v, Encoding e) {
  if (e == Encoding::NEG_MAGNITUDE) return (uint16_t) (v < 0 ? -v : v);
  return (uint16_t) (int16_t) v;
}

class RegisterStore {
 public:
  static constexpr uint16_t REG_MAX = 4096;  // this protocol uses 1000..2239; 4096 leaves headroom
  static constexpr int ENTRIES_PER_LINE = 8;

  enum class DumpKind : uint8_t { NONE, CHANGES, NEW_UNMAPPED };

  struct Update {
    bool stored;      // false: register >= REG_MAX, ignored
    bool first_seen;  // first time this register has carried a value since boot
    bool changed;     // value differs from the previous one (always true when first_seen)
    uint16_t old_value;
  };

  void set_mapped(uint16_t r) {
    if (r < REG_MAX) mapped_.set(r);
  }
  bool is_mapped(uint16_t r) const { return r < REG_MAX && mapped_.test(r); }
  bool has(uint16_t r) const { return r < REG_MAX && known_.test(r); }
  uint16_t get(uint16_t r) const { return r < REG_MAX ? regs_[r] : 0; }

  Update update(uint16_t r, uint16_t v) {
    if (r >= REG_MAX) {
      ignored_high_++;
      return {false, false, false, 0};
    }
    Update u{true, !known_.test(r), true, regs_[r]};
    if (!u.first_seen) u.changed = regs_[r] != v;
    regs_[r] = v;
    if (u.first_seen) {
      known_.set(r);
      known_count_++;
      if (!mapped_.test(r)) {
        unmapped_count_++;
        new_unmapped_frame_.push_back(r);
      }
    }
    return u;
  }

  uint16_t known_count() const { return known_count_; }
  uint16_t unmapped_count() const { return unmapped_count_; }
  uint32_t ignored_high_count() const { return ignored_high_; }

  // Unmapped registers first seen since the previous call. Used to log one coalesced
  // line per frame, so a first sighting is visible in the log without pressing anything.
  std::vector<uint16_t> take_new_unmapped() {
    std::vector<uint16_t> out;
    out.swap(new_unmapped_frame_);
    return out;
  }

  // "count: ranges" of every unmapped register seen since boot, at most max_len chars
  // (Home Assistant's text-sensor limit is 255).
  std::string unmapped_summary(size_t max_len = 255) const {
    if (unmapped_count_ == 0) return "none";
    std::vector<uint16_t> all;
    all.reserve(unmapped_count_);
    for (uint16_t r = 0; r < REG_MAX; r++)
      if (known_.test(r) && !mapped_.test(r)) all.push_back(r);
    char head[16];
    snprintf(head, sizeof(head), "%u: ", (unsigned) all.size());
    std::string out = head;
    size_t omitted = 0;
    size_t budget = max_len > out.size() + 20 ? max_len - out.size() - 20 : 0;
    out += reg_ranges::format(all, budget, &omitted);
    if (omitted) {
      char tail[20];
      snprintf(tail, sizeof(tail), " (+%u more)", (unsigned) omitted);
      out += tail;
    }
    if (out.size() > max_len) out.resize(max_len);
    return out;
  }

  // ---------------------------------------------------------------------------------
  // Incremental dumps: begin_dump(), then call dump_step() once per loop() until it
  // returns false, logging `line` whenever it is non-empty, then log dump_summary().
  // Spreading it out keeps the logger/API buffer from being flooded.
  // ---------------------------------------------------------------------------------
  bool dump_active() const { return dump_kind_ != DumpKind::NONE; }
  DumpKind dump_kind() const { return dump_kind_; }

  bool begin_dump(DumpKind k) {
    if (dump_active() || k == DumpKind::NONE) return false;
    dump_kind_ = k;
    dump_pos_ = 0;
    d_listed_ = 0;
    d_first_seen_ = 0;
    return true;
  }

  // Fills `line` with up to ENTRIES_PER_LINE entries (may be empty). Returns true while
  // there is more to do, false once the dump has finished.
  bool dump_step(std::string &line) {
    line.clear();
    if (!dump_active()) return false;
    int entries = 0;
    while (dump_pos_ < REG_MAX && entries < ENTRIES_PER_LINE) {
      uint16_t r = dump_pos_++;
      if (dump_kind_ == DumpKind::CHANGES) {
        // Each register's baseline is updated as the scan passes it, so a change that
        // happens mid-dump is either listed now or at the next press -- never lost.
        bool now_known = known_.test(r), was_known = snap_known_.test(r);
        if (now_known && was_known && snap_[r] != regs_[r]) {
          append_change_(line, r, snap_[r], regs_[r]);
          entries++;
          d_listed_++;
        } else if (now_known && !was_known) {
          d_first_seen_++;
        }
        snap_[r] = regs_[r];
        if (now_known) snap_known_.set(r);
      } else {  // NEW_UNMAPPED
        if (known_.test(r) && !mapped_.test(r) && !unmapped_dumped_.test(r)) {
          append_value_(line, r, regs_[r]);
          unmapped_dumped_.set(r);
          entries++;
          d_listed_++;
        }
      }
    }
    if (dump_pos_ >= REG_MAX) {
      if (dump_kind_ == DumpKind::CHANGES) {
        d_was_baseline_ = !changes_baseline_taken_;
        changes_baseline_taken_ = true;
      }
      d_finished_kind_ = dump_kind_;
      dump_kind_ = DumpKind::NONE;
      return false;
    }
    return true;
  }

  // One line describing the dump that just finished.
  std::string dump_summary() const {
    char buf[220];
    if (d_finished_kind_ == DumpKind::CHANGES) {
      if (d_was_baseline_)
        snprintf(buf, sizeof(buf),
                 "first press since boot: baseline of %u register(s) recorded, nothing to compare yet. "
                 "Press again later to see what changed.",
                 (unsigned) known_count_);
      else
        snprintf(buf, sizeof(buf),
                 "%u register(s) changed since the last press; %u register(s) seen for the first time "
                 "since then (not listed: no previous value)",
                 (unsigned) d_listed_, (unsigned) d_first_seen_);
    } else if (d_finished_kind_ == DumpKind::NEW_UNMAPPED) {
      snprintf(buf, sizeof(buf),
               "%u unmapped register(s) new since the last press (unmapped seen since boot: %u)",
               (unsigned) d_listed_, (unsigned) unmapped_count_);
    } else {
      buf[0] = '\0';
    }
    return buf;
  }

 private:
  // Values with the top bit set are shown both ways, e.g. 64536(-1000), since some
  // registers are signed and some are not.
  static void append_num_(std::string &s, uint16_t v) {
    char b[16];
    if (v & 0x8000)
      snprintf(b, sizeof(b), "%u(%d)", (unsigned) v, (int) (int16_t) v);
    else
      snprintf(b, sizeof(b), "%u", (unsigned) v);
    s += b;
  }
  static void append_change_(std::string &s, uint16_t r, uint16_t a, uint16_t b) {
    char h[12];
    snprintf(h, sizeof(h), "%u: ", (unsigned) r);
    s += h;
    append_num_(s, a);
    s += "->";
    append_num_(s, b);
    s += "  ";
  }
  static void append_value_(std::string &s, uint16_t r, uint16_t v) {
    char h[12];
    snprintf(h, sizeof(h), "%u=", (unsigned) r);
    s += h;
    append_num_(s, v);
    s += "  ";
  }

  uint16_t regs_[REG_MAX]{};
  std::bitset<REG_MAX> known_, mapped_;
  uint16_t known_count_{0}, unmapped_count_{0};
  uint32_t ignored_high_{0};
  std::vector<uint16_t> new_unmapped_frame_;

  // CHANGES button memory
  uint16_t snap_[REG_MAX]{};
  std::bitset<REG_MAX> snap_known_;
  bool changes_baseline_taken_{false};
  // NEW_UNMAPPED button memory
  std::bitset<REG_MAX> unmapped_dumped_;

  // dump in progress / last finished
  DumpKind dump_kind_{DumpKind::NONE}, d_finished_kind_{DumpKind::NONE};
  uint16_t dump_pos_{0};
  uint16_t d_listed_{0}, d_first_seen_{0};
  bool d_was_baseline_{false};
};

}  // namespace thermia
