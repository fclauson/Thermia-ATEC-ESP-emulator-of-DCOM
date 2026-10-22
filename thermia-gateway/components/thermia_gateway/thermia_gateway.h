// thermia_gateway.h  v2026.10.20.5  ESP gateway-replacement component (header)
#pragma once
// Thermia / Danfoss DHP-AQ: ESP replacement for the Thermia Online gateway (0x0F).
//
// The ESP sits on the RS485 bus in the gateway's place and, once ARMED, answers the heat
// pump as the gateway: it acks the heat pump's writes, serves its reads from a store seeded
// only by what the heat pump itself sent this session, serves the mailbox (1800-1805), and
// changes settings by staging one register and raising that block's mailbox re-read flag.
// It decodes every register the two exchange and publishes the mapped ones as sensors.
//
// Safety: DISARMED at every boot, driver-enable (de_pin) held low except while an armed
// reply is transmitted, auto-disarm within ~50 ms if a real gateway is heard. With
// auto_takeover it self-arms after auto_listen_ms of no real gateway, then seeds its store
// with one full resync. A register not seen since boot is never served or published (Home
// Assistant shows it as "unknown"); there is no flash persistence and no default value.
//
// Also decoded, receive-only:
//   - the heat pump's ~1 s FC23 exchanges with its other bus devices (0x02, 0x04, ...),
//     read through has_cmd()/get_cmd() and has_reply()/get_reply();
//   - alarm bits 2143..2147 with their firmware names (alarm_bits.h);
//   - handshake check: the CM930 answer is computed for every heat-pump challenge and
//     compared with a real gateway's reply, if one is connected (cm930_handshake.h);
//   - gateway shadow: the reply the ESP would send, compared with a real gateway's reply
//     (gateway_shadow.h). Both checks only produce results while a real gateway is present.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/gpio.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "modbus_rtu.h"
#include "cm930_handshake.h"
#include "alarm_bits.h"
#include "gateway_shadow.h"
#include "gateway_responder.h"
#include "register_store.h"
#include "registers_json.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace web_server_base {
class WebServerBase;
}
namespace thermia_gateway {

class ThermiaGateway : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_slave_address(uint8_t a) {
    this->decoder_ = modbus_rtu::Decoder(a);
    this->hs_ = cm930::HandshakeChecker(a);
    this->shadow_.set_addr(a);
    this->responder_.set_addr(a);
  }
  void set_frame_gap_us(uint32_t g) { this->framer_ = modbus_rtu::Framer(g, 30000); }
  void set_log_changes(bool v) { this->log_changes_ = v; }
  void add_register(uint16_t reg, bool is_signed, sensor::Sensor *s) {
    this->sensors_.push_back({reg, is_signed, s});
    this->store_.set_mapped(reg);
  }

  // --- Buttons (output goes to the ESPHome log, tag thermia_dump) ---
  // "Log Changed Registers Since Last Logged"
  void log_changed_since_last();
  // "Dump Unmapped Registers": unmapped registers that are new since the last press.
  void dump_unmapped_new();

  // --- For YAML template sensors ---
  // has_register() is false until the register has actually been seen since boot;
  // lambdas return {} in that case so the entity stays "unknown".
  bool has_register(uint16_t reg) const { return this->store_.has(reg); }
  uint16_t get_register(uint16_t reg) const { return this->store_.get(reg); }
  std::string unmapped_summary() const { return this->store_.unmapped_summary(); }
  // True if the gateway (0x0F) acknowledged a write or answered a read/handshake in the last 15 s.
  bool gateway_replying() const;

  // --- Handshake check (computed answer vs the real gateway's reply; never transmitted) ---
  // Text for HA, e.g. "MATCH (1 of 1 checked)", "MISMATCH ...", "no gateway reply ...",
  // "waiting for gateway reply", "none seen since ESP boot".
  std::string handshake_status() const;
  uint32_t handshake_requests() const { return this->hs_.requests(); }
  uint32_t handshake_matches() const { return this->hs_.matches(); }
  uint32_t handshake_mismatches() const { return this->hs_.mismatches(); }

  // --- Alarm bits 2143..2147 ---
  // alarms_known(): at least one of 2143..2145 has been seen since the ESP booted (the
  // heat pump writes this block at its boot and, presumably, when an alarm changes).
  bool alarms_known() const;
  bool any_alarm_bit() const;       // any bit set in 2143..2147 (raw, conditions not applied)
  std::string active_alarms() const;  // "None", or "2144.13 compressor_error/..., ..." (<=250 chars)

  // --- Gateway shadow: the reply the ESP would send vs a real gateway's, compared
  // byte-for-byte. Only active while a real gateway is connected. See gateway_shadow.h. ---
  // "OK N" while every checked reply matched; otherwise the last mismatch, with counts.
  std::string shadow_status() const;
  uint32_t shadow_ack_mismatches() const { return this->shadow_.ack_mismatch(); }
  uint32_t shadow_read_changes() const { return this->shadow_.read_change(); }
  uint32_t shadow_mailbox_events() const { return this->shadow_.mailbox_event(); }

  // --- Takeover: answer AS the gateway. TRANSMITS only when armed. ---
  // Arming goes through try_arm_ (every gate must pass); any reboot disarms. A reply is sent
  // only when armed, only while no real gateway is replying, and only into an idle bus.
  void set_flow_control_pin(GPIOPin *p) { this->de_pin_ = p; }
  void set_takeover_capable(bool v) { this->takeover_capable_ = v; }  // YAML opt-in, default false
  // Auto-takeover: the ESP arms itself after listening for a real gateway for auto_listen_ms
  // and hearing none, then requests one full resync to seed its store. Auto-disarm on hearing
  // a real gateway stays active throughout, so replugging the real gateway always makes the
  // ESP step aside. Default off.
  void set_auto_takeover(bool v) { this->auto_takeover_ = v; }
  void set_auto_listen_ms(uint32_t ms) { this->auto_listen_ms_ = ms; }
  void request_takeover(bool on);
  bool takeover_armed() const { return this->armed_; }
  std::string takeover_status() const;
  uint32_t tx_frames_sent() const { return this->tx_sent_; }

  // Text of the last write / resync outcome, for the "Last Write Result" sensor.
  std::string last_write_result() const { return this->last_write_result_; }

  // --- Full resync (armed only): serves the resync mailbox pattern so the heat pump re-pushes
  // its settings table (all of 1000-1777; NOT 1780-1798). See full-resync-trigger.md. This is
  // how the store is seeded when the ESP takes over without having watched a heat-pump boot. ---
  void request_full_resync();
  // Seconds since the heat pump last wrote any register to the gateway address (FC16).
  // Shows whether the heat pump is still actively talking to us. NAN if never seen.
  float seconds_since_hp_update() const;
  // Mailbox reads the heat pump requested, served, and skipped because the bus was busy.
  // Dropped reads delay flags reaching the heat pump.
  uint32_t mailbox_requested() const { return this->mbx_req_; }
  uint32_t mailbox_served() const { return this->mbx_served_; }
  uint32_t mailbox_dropped_idle() const { return this->mbx_dropped_; }

  // --- Writing a setting: clamp to [lo,hi], change exactly one register in the store, and
  // raise the block's mailbox flag twice (pulse, gap, pulse) so the heat pump re-reads it.
  // flag_value is the block's bit in 1801 (all 16 confirmed): 1 -> 1000, 4 -> 1040,
  // 8 -> 1070, ... Refused unless armed AND the register has been seen this session.
  // set_writable_reg refuses any negative range (lo < 0): use set_writable_reg_signed.
  // `label` is only for the log and the result text.
  void set_writable_reg(uint16_t reg, float value, int lo, int hi, uint16_t flag_value, const char *label);
  // Negative-range register with a bus encoding confirmed by reading it (see thermia::Encoding).
  // value/lo/hi in display units. Same gates, flag and two-pulse delivery as set_writable_reg.
  void set_writable_reg_signed(uint16_t reg, float value, int lo, int hi, uint16_t flag_value, const char *label,
                               thermia::Encoding enc);
  // Upper settings register (1350-1798): same as set_writable_reg but the block flag goes in
  // mailbox word 1800 (W0). Confirmed: 1800=1 -> block 1350-1369 (operation mode 1363).
  void set_writable_reg_upper(uint16_t reg, float value, int lo, int hi, uint16_t flag_value, const char *label);
  // --- GET /registers.json: every register seen + fresh live I/O, raw unsigned 16-bit.
  // Built in the main loop every JSON_REFRESH_MS; the web handler only copies the text. ---
  void set_web_base(web_server_base::WebServerBase *base) { this->web_base_ = base; }
  std::string registers_json();  // thread-safe copy of the last JSON built (called by the handler)

  float get_reg_value(uint16_t reg) const;  // current value in store, NAN if unseen

  // --- Live I/O: the heat pump's own FC23 exchanges with its other bus devices ---
  // These arrive about once a second and do not depend on the gateway. A value counts
  // as fresh only if it was seen within max_age_ms; lambdas return {} / NAN otherwise.
  //   has_cmd/get_cmd     : what the heat pump WROTE to device dev at address addr
  //   has_reply/get_reply : what device dev RETURNED for address addr
  // Addresses are the device's own (e.g. dev 2, 0xA80C), not gateway register numbers.
  static constexpr uint32_t LIVE_MAX_AGE_MS = 5000;
  bool has_cmd(uint8_t dev, uint16_t addr, uint32_t max_age_ms = LIVE_MAX_AGE_MS) const {
    return this->live_fresh_(this->live_key_(false, dev, addr), max_age_ms);
  }
  uint16_t get_cmd(uint8_t dev, uint16_t addr) const { return this->live_get_(this->live_key_(false, dev, addr)); }
  bool has_reply(uint8_t dev, uint16_t addr, uint32_t max_age_ms = LIVE_MAX_AGE_MS) const {
    return this->live_fresh_(this->live_key_(true, dev, addr), max_age_ms);
  }
  uint16_t get_reply(uint8_t dev, uint16_t addr) const { return this->live_get_(this->live_key_(true, dev, addr)); }

 protected:
  struct RegSensor {
    uint16_t reg;
    bool is_signed;
    sensor::Sensor *sensor;
  };

  // Single entry point for received bytes (main RS485 port).
  void feed_byte_(uint8_t b, uint32_t now_us);
  void handle_frame_(const uint8_t *d, size_t n);
  void on_register_(uint16_t reg, uint16_t value);
  void report_new_unmapped_();
  void audit_(const uint8_t *d, size_t n);
  void start_dump_(thermia::RegisterStore::DumpKind k, const char *title);
  void on_live_(bool reply, uint8_t dev, uint16_t addr, uint16_t value);

  struct LiveVal {
    uint16_t value;
    uint32_t ms;
  };
  static uint32_t live_key_(bool reply, uint8_t dev, uint16_t addr) {
    return ((uint32_t) (reply ? 1 : 0) << 24) | ((uint32_t) dev << 16) | addr;
  }
  bool live_fresh_(uint32_t key, uint32_t max_age_ms) const;
  uint16_t live_get_(uint32_t key) const;

  HighFrequencyLoopRequester high_freq_;
  modbus_rtu::Framer framer_{3000, 30000};
  modbus_rtu::Decoder decoder_{0x0F};
  modbus_rtu::Fc23Decoder live_decoder_;
  std::map<uint32_t, LiveVal> live_;  // ~60 entries on this bus; capped at LIVE_MAX_ENTRIES
  static constexpr size_t LIVE_MAX_ENTRIES = 512;
  bool live_full_warned_{false};

  // /registers.json cache (see registers_json.h / registers_json_handler.h)
  static constexpr uint32_t JSON_REFRESH_MS = 30000;
  static constexpr uint32_t JSON_LIVE_MAX_AGE_MS = 60000;  // live values older than this are left out
  void refresh_registers_json_();
  web_server_base::WebServerBase *web_base_{nullptr};
  bool web_started_{false};  // web server started on the first loop() pass, never in setup()
  std::mutex json_mutex_;
  std::string json_cache_;
  uint32_t json_built_ms_{0};
  thermia::RegisterStore store_;
  std::vector<RegSensor> sensors_;
  struct PendingReadback { uint16_t reg; uint16_t value; bool done; };
  std::vector<PendingReadback> pending_readback_;  // writes awaiting heat-pump read-back
  bool log_changes_{false};
  uint32_t bytes_{0};
  bool high_warned_{false};

  // Audit counters: every CRC-valid frame is classified so nothing goes unnoticed.
  uint32_t a_w_ok_{0}, a_w_ack_{0}, a_w_bad_{0};                     // FC10 to 0x0F: decoded / ack / malformed
  uint32_t a_r_req_{0}, a_r_ok_{0}, a_r_unmatched_{0}, a_r_bad_{0};  // FC03 to/from 0x0F
  uint32_t a_17_{0}, a_exc_{0}, a_other_fc_{0};                      // FC17, error replies, other codes
  uint32_t a_06_{0};                                                 // FC06 frames seen (raw count)
  uint32_t a_other_dev_{0};                                          // frames for other bus devices
  uint32_t a_logged_{0};
  uint16_t a_rq_qty_{0};

  // Gateway health
  uint32_t last_gw_reply_ms_{0};
  bool has_gw_reply_{false};
  bool gw_state_{false}, gw_known_{false};

  // Handshake check (FC17): the computed answer is only logged and compared, never sent.
  enum class HsResult : uint8_t { NONE, PENDING, MATCH, MISMATCH, NO_REPLY };
  void audit_handshake_(const uint8_t *d, size_t n, bool &gw_reply, const char *&why);
  void handshake_timeout_check_();
  cm930::HandshakeChecker hs_{0x0F};
  cm930::GatewayShadow shadow_{0x0F};
  void audit_shadow_(const uint8_t *d, size_t n);
  std::string shadow_last_;  // last non-OK report, for shadow_status()

  // --- Takeover state (every default is the safe/off value) ---
  cm930::GatewayResponder responder_{0x0F};
  GPIOPin *de_pin_{nullptr};       // RS485 driver-enable; nullptr = cannot transmit at all
  bool takeover_capable_{false};   // YAML opt-in; false = arming always refused
  bool armed_{false};              // live transmit enable; cleared on boot and on any fault
  // True once a heat-pump BOOT push has ended (its last block, 1780). A resync never sends
  // 1780, so after a resync-seeded takeover this stays false; that is expected.
  bool table_seen_this_session_{false};
  uint32_t last_table_push_ms_{0};
  uint32_t last_gw_frame_ms_{0};   // last time we heard the REAL gateway reply
  bool heard_gw_this_session_{false};
  uint32_t tx_sent_{0};
  const char *arm_refused_{nullptr};
  const char *disarm_reason_{nullptr};
  std::string last_write_result_{"none"};
  // Auto-takeover state
  bool auto_takeover_{false};
  uint32_t auto_listen_ms_{20000};
  bool auto_arm_done_{false};        // we have already made the auto-arm decision this boot
  bool manual_disarm_latch_{false};  // user turned the toggle OFF; blocks auto-arm until reboot
  bool auto_resync_done_{false};     // we have auto-requested a seed resync this session
  bool gw_addressed_seen_{false};    // the HP has addressed the gateway at least once this session
  uint32_t boot_ms_{0};
  uint32_t last_hp_update_ms_{0};
  bool hp_update_seen_{false};
  uint32_t mbx_req_{0}, mbx_served_{0}, mbx_dropped_{0};
  void auto_takeover_tick_();
  void publish_reg_(uint16_t reg, uint16_t value);
  void publish_sensors_(uint16_t reg, uint16_t value);  // push to the mapped sensor(s), if any
  bool reg_is_signed_(uint16_t reg) const;  // the register's YAML `signed:` setting
  void stage_reg_write_(uint16_t reg, uint16_t raw, int shown, uint16_t flag_idx, uint16_t flag_value,
                        const char *label);
  void mark_pending_readback_(uint16_t reg, uint16_t value);
  void confirm_readbacks_(const uint8_t *d, size_t n);
  void try_arm_();
  void maybe_respond_(const uint8_t *d, size_t n);
  void send_frame_(const uint8_t *b, size_t len);
  void force_disarm_(const char *why);
  HsResult hs_last_{HsResult::NONE};
  uint32_t hs_req_ms_{0};
  uint32_t hs_last_reply_ms_{0};  // request -> reply time of the last answered handshake
  static constexpr uint32_t HS_REPLY_TIMEOUT_MS = 1000;  // real gateway: ~29 ms
};

}  // namespace thermia_gateway
}  // namespace esphome
