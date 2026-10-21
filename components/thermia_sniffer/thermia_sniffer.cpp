// thermia_sniffer.cpp  v2026.10.20.5  ESP gateway-replacement component (impl)
#include <cmath>
#include "thermia_sniffer.h"
#include "registers_json_handler.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>

namespace esphome {
namespace thermia_sniffer {

static const char *const TAG = "thermia_sniffer";
static const char *const TAG_DUMP = "thermia_dump";
static const char *const TAG_UNMAPPED = "thermia_unmapped";
static const char *const TAG_AUDIT = "thermia_audit";

using DumpKind = thermia::RegisterStore::DumpKind;

void ThermiaSniffer::setup() {
  ESP_LOGI(TAG, "every sensor is unknown until the heat pump sends that register");
  // DISARMED at every boot: driver-enable is an output held LOW (receive), so the board
  // cannot drive the bus until armed.
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);
  }
  this->armed_ = false;
  this->boot_ms_ = millis();
  // The /registers.json web server is NOT started here: this component sets up before WiFi,
  // and starting an HTTP server before the network stack exists aborts the ESP (boot loop,
  // OTA rollback). It is started on the first loop() pass, after every component's setup().
  ESP_LOGW(TAG, "takeover: DISARMED at boot (capable=%s, auto=%s). Transmit is impossible until armed.",
           YESNO(this->takeover_capable_), YESNO(this->auto_takeover_));
  if (this->auto_takeover_)
    ESP_LOGW(TAG, "auto-takeover ON: will listen %u s for a real gateway, then self-arm if none heard",
             (unsigned) (this->auto_listen_ms_ / 1000));
  this->high_freq_.start();  // fast loop so idle-gap framing is accurate
  this->set_interval("gw_state", 5000, [this]() {
    bool now = this->gateway_replying();
    if (!this->gw_known_ || now != this->gw_state_) {
      this->gw_known_ = true;
      this->gw_state_ = now;
      ESP_LOGW(TAG, "gateway 0x%02X is %s", this->decoder_.addr(),
               now ? "REPLYING" : "NOT replying (no ack/answer in the last 15 s)");
    }
  });
  // A handshake with no gateway reply is reported once the gateway is clearly too late.
  this->set_interval("hs_timeout", 250, [this]() { this->handshake_timeout_check_(); });
  this->set_interval("stats", 60000, [this]() {
    ESP_LOGI(TAG, "bytes=%u frames ok=%u bad=%u | registers seen=%u (unmapped %u)", (unsigned) this->bytes_,
             (unsigned) this->framer_.ok(), (unsigned) this->framer_.bad(), (unsigned) this->store_.known_count(),
             (unsigned) this->store_.unmapped_count());
    ESP_LOGI(TAG_AUDIT, "live I/O (FC23 to other devices): requests=%u replies=%u addresses=%u",
             (unsigned) this->live_decoder_.requests(), (unsigned) this->live_decoder_.replies(),
             (unsigned) this->live_.size());
    ESP_LOGI(TAG_AUDIT,
             "0x%02X: FC10 write=%u ack=%u (unacked=%d) malformed=%u | FC03 req=%u reply=%u unmatched=%u "
             "malformed=%u | NOT DECODED: FC17=%u FC06=%u error-reply=%u other-fc=%u | other-device frames=%u",
             this->decoder_.addr(), (unsigned) this->a_w_ok_, (unsigned) this->a_w_ack_,
             (int) this->a_w_ok_ - (int) this->a_w_ack_, (unsigned) this->a_w_bad_, (unsigned) this->a_r_req_,
             (unsigned) this->a_r_ok_, (unsigned) this->a_r_unmatched_, (unsigned) this->a_r_bad_,
             (unsigned) this->a_17_, (unsigned) this->a_06_, (unsigned) this->a_exc_, (unsigned) this->a_other_fc_,
             (unsigned) this->a_other_dev_);
    ESP_LOGI(TAG_AUDIT, "handshake check (computed answer vs real gateway): requests=%u match=%u MISMATCH=%u no-reply=%u",
             (unsigned) this->hs_.requests(), (unsigned) this->hs_.matches(), (unsigned) this->hs_.mismatches(),
             (unsigned) (this->hs_.unanswered() + (this->hs_last_ == HsResult::NO_REPLY && this->hs_.pending() ? 1 : 0)));
    ESP_LOGI(TAG_AUDIT,
             "gateway shadow (predicted reply vs real gateway): checked=%u OK=%u | ACK-MISMATCH=%u "
             "read-change=%u mailbox-event=%u read-fail=%u",
             (unsigned) this->shadow_.replies_checked(), (unsigned) this->shadow_.ok(),
             (unsigned) this->shadow_.ack_mismatch(), (unsigned) this->shadow_.read_change(),
             (unsigned) this->shadow_.mailbox_event(), (unsigned) this->shadow_.read_fail());
  });
}

void ThermiaSniffer::loop() {
  uint8_t b;
  uint32_t now = micros();
  this->responder_.tick(millis());  // expire any resync / write-flag hold by wall clock
  this->auto_takeover_tick_();
  if (this->web_base_ != nullptr) {
    if (!this->web_started_) {  // first loop pass: all setup() done, network stack exists
      this->web_started_ = true;
      this->web_base_->init();
      this->web_base_->add_handler(new RegistersJsonHandler(this));  // lives for the whole run
      ESP_LOGI(TAG, "serving GET /registers.json (raw registers + live I/O, refreshed every %u s)",
               (unsigned) (JSON_REFRESH_MS / 1000));
    }
    if (this->json_built_ms_ == 0 || millis() - this->json_built_ms_ >= JSON_REFRESH_MS)
      this->refresh_registers_json_();
  }
  while (this->available()) {
    if (this->read_byte(&b)) this->feed_byte_(b, now);
  }

  if (this->store_.dump_active()) {
    std::string line;
    bool more = this->store_.dump_step(line);
    if (!line.empty()) ESP_LOGW(TAG_DUMP, "%s", line.c_str());
    if (!more) {
      ESP_LOGW(TAG_DUMP, "%s", this->store_.dump_summary().c_str());
      ESP_LOGW(TAG_DUMP, "THERMIA-DUMP END");
    }
  }

  const uint8_t *d;
  size_t n;
  if (this->framer_.poll(micros(), &d, &n)) this->handle_frame_(d, n);
}

void ThermiaSniffer::feed_byte_(uint8_t b, uint32_t now_us) {
  this->bytes_++;
  this->framer_.push(b, now_us);
}

void ThermiaSniffer::handle_frame_(const uint8_t *d, size_t n) {
  ESP_LOGV(TAG, "frame: %s", format_hex_pretty(d, n).c_str());
  this->audit_(d, n);
  this->audit_shadow_(d, n);
  this->responder_.observe(d, n);   // seed the responder store from real writes (always)
  if (n >= 1 && d[0] == this->decoder_.addr()) this->gw_addressed_seen_ = true;  // HP is talking to us
  this->confirm_readbacks_(d, n);   // reflect a value we wrote once the heat pump re-reads its block
  this->maybe_respond_(d, n);        // answer AS the gateway, only if armed
  this->decoder_.handle(d, n, [this](uint16_t r, uint16_t v) { this->on_register_(r, v); });
  if (n >= 1 && d[0] != this->decoder_.addr()) {  // gateway FC23 is the handshake, not I/O
    this->live_decoder_.handle(
        d, n, [this](uint8_t dev, uint16_t a, uint16_t v) { this->on_live_(false, dev, a, v); },
        [this](uint8_t dev, uint16_t a, uint16_t v) { this->on_live_(true, dev, a, v); });
  }
  this->report_new_unmapped_();  // one coalesced line per frame, only if something new appeared
}

// Publish a register's display sensor and update the main store to `value`. Used both for heat
// pump writes (on_register_) and to reflect a setting WE wrote once the heat pump has re-read it.
void ThermiaSniffer::publish_reg_(uint16_t reg, uint16_t value) {
  this->store_.update(reg, value);
  this->publish_sensors_(reg, value);
}

void ThermiaSniffer::publish_sensors_(uint16_t reg, uint16_t value) {
  for (auto &s : this->sensors_)
    if (s.reg == reg) s.sensor->publish_state(thermia::decode_value(value, s.is_signed));
}

// Remember that we wrote `value` to `reg` and are waiting for the heat pump to re-read it, so we
// can then show the adopted value on the display sensor (only-valid-data: not shown until read).
void ThermiaSniffer::mark_pending_readback_(uint16_t reg, uint16_t value) {
  for (auto &p : this->pending_readback_)
    if (p.reg == reg) { p.value = value; p.done = false; return; }
  this->pending_readback_.push_back({reg, value, false});
}

// When the heat pump issues an FC03 read of a settings block (addressed to us, not the mailbox),
// any pending-write register inside [start, start+qty) is being adopted right now. Publish the
// value we served for it to the display sensor and retire the pending entry.
void ThermiaSniffer::confirm_readbacks_(const uint8_t *d, size_t n) {
  if (this->pending_readback_.empty()) return;
  uint16_t start = 0, qty = 0;
  if (!cm930::GatewayResponder::is_settings_block_read(d, n, this->decoder_.addr(), start, qty))
    return;  // only a settings-block FC03 read (not the mailbox) confirms adoption
  for (auto &p : this->pending_readback_) {
    if (!p.done && p.reg >= start && p.reg < (uint16_t) (start + qty)) {
      this->publish_reg_(p.reg, this->responder_.get(p.reg));  // committed value the HP just read
      p.done = true;
      ESP_LOGD(TAG, "reg %u read back by heat pump (=%u) - display updated", (unsigned) p.reg,
               (unsigned) this->responder_.get(p.reg));
    }
  }
  // drop retired entries
  this->pending_readback_.erase(
      std::remove_if(this->pending_readback_.begin(), this->pending_readback_.end(),
                     [](const PendingReadback &p) { return p.done; }),
      this->pending_readback_.end());
}

void ThermiaSniffer::on_register_(uint16_t reg, uint16_t value) {
  this->last_hp_update_ms_ = millis();   // the heat pump just wrote a register to us
  this->hp_update_seen_ = true;
  auto u = this->store_.update(reg, value);
  if (!u.stored) {
    if (!this->high_warned_) {
      this->high_warned_ = true;
      ESP_LOGW(TAG_UNMAPPED, "register %u is beyond what is stored (>= %u) - ignored (further ones counted only)",
               (unsigned) reg, (unsigned) thermia::RegisterStore::REG_MAX);
    }
    return;
  }
  if (!u.changed) return;
  if (!u.first_seen && this->log_changes_)
    ESP_LOGD(TAG, "reg %u: %u -> %u (signed %d)", reg, u.old_value, value, (int16_t) value);
  this->publish_sensors_(reg, value);
}

void ThermiaSniffer::on_live_(bool reply, uint8_t dev, uint16_t addr, uint16_t value) {
  const uint32_t key = live_key_(reply, dev, addr);
  auto it = this->live_.find(key);
  if (it == this->live_.end()) {
    if (this->live_.size() >= LIVE_MAX_ENTRIES) {
      if (!this->live_full_warned_) {
        this->live_full_warned_ = true;
        ESP_LOGW(TAG, "live I/O table full (%u entries) - new addresses ignored", (unsigned) LIVE_MAX_ENTRIES);
      }
      return;
    }
    this->live_[key] = {value, millis()};
    return;
  }
  if (it->second.value != value && this->log_changes_)
    ESP_LOGD(TAG, "live dev%u %s 0x%04X: %u -> %u", (unsigned) dev, reply ? "reply" : "cmd", (unsigned) addr,
             (unsigned) it->second.value, (unsigned) value);
  it->second.value = value;
  it->second.ms = millis();
}

// Main loop only: snapshot the store and the fresh live values into JSON text. The web task
// reads the copy, never the store or the live map (which the main loop changes).
void ThermiaSniffer::refresh_registers_json_() {
  const uint32_t now = millis();
  std::vector<thermia::LiveEntry> live;
  live.reserve(this->live_.size());
  for (const auto &kv : this->live_) {
    if (now - kv.second.ms > JSON_LIVE_MAX_AGE_MS) continue;
    const uint32_t k = kv.first;
    live.push_back({((k >> 24) & 1) != 0, (uint8_t) ((k >> 16) & 0xFF), (uint16_t) (k & 0xFFFF), kv.second.value});
  }
  std::string body = thermia::build_registers_json(this->store_, live, (now - this->boot_ms_) / 1000, this->armed_);
  {
    std::lock_guard<std::mutex> lock(this->json_mutex_);
    this->json_cache_.swap(body);
  }
  this->json_built_ms_ = now == 0 ? 1 : now;
}

std::string ThermiaSniffer::registers_json() {
  std::lock_guard<std::mutex> lock(this->json_mutex_);
  return this->json_cache_.empty() ? std::string("{}") : this->json_cache_;
}

bool ThermiaSniffer::live_fresh_(uint32_t key, uint32_t max_age_ms) const {
  auto it = this->live_.find(key);
  return it != this->live_.end() && (millis() - it->second.ms) <= max_age_ms;
}

uint16_t ThermiaSniffer::live_get_(uint32_t key) const {
  auto it = this->live_.find(key);
  return it == this->live_.end() ? 0 : it->second.value;
}

void ThermiaSniffer::report_new_unmapped_() {
  auto fresh = this->store_.take_new_unmapped();
  if (fresh.empty()) return;
  std::sort(fresh.begin(), fresh.end());
  size_t omitted = 0;
  std::string ranges = reg_ranges::format(fresh, 280, &omitted);
  ESP_LOGW(TAG_UNMAPPED, "first sighting of %u register(s) with no sensor in the YAML: %s%s (unmapped total: %u)",
           (unsigned) fresh.size(), ranges.c_str(),
           omitted ? str_sprintf(" ... +%u more", (unsigned) omitted).c_str() : "",
           (unsigned) this->store_.unmapped_count());
}

// ---------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------
void ThermiaSniffer::start_dump_(DumpKind k, const char *title) {
  if (!this->store_.begin_dump(k)) {
    ESP_LOGW(TAG_DUMP, "a dump is already running - press again when it has finished");
    return;
  }
  ESP_LOGW(TAG_DUMP, "THERMIA-DUMP START: %s", title);
}

void ThermiaSniffer::log_changed_since_last() {
  this->start_dump_(DumpKind::CHANGES, "registers changed since the last press (reg: old->new)");
}

void ThermiaSniffer::dump_unmapped_new() {
  this->start_dump_(DumpKind::NEW_UNMAPPED, "unmapped registers new since the last press (reg=value)");
}

// ---------------------------------------------------------------------------------------
// Audit: classifies every CRC-valid frame so we can prove what is (and is not) decoded.
// Anything the decoder cannot use is logged at WARN (first 30 per boot) with its raw bytes.
// ---------------------------------------------------------------------------------------
void ThermiaSniffer::audit_(const uint8_t *d, size_t n) {
  if (n < 4) return;
  if (d[0] != this->decoder_.addr()) {
    this->a_other_dev_++;
    return;
  }
  const uint8_t fc = d[1];
  const char *why = nullptr;
  bool gw_reply = false;
  if (fc == 0x10) {
    uint16_t qty = (uint16_t) ((d[4] << 8) | d[5]);
    if (n == 8) {
      this->a_w_ack_++;
      gw_reply = true;
    } else if (n >= 11 && n == (size_t) d[6] + 9 && d[6] == qty * 2 && qty > 0) {
      this->a_w_ok_++;
      // Boot-push completion: a heat-pump boot push ends with the 1780 block. (A resync push
      // never includes 1780, so this only marks a real heat-pump boot.)
      uint16_t wstart = (uint16_t) ((d[2] << 8) | d[3]);
      if (wstart == 1780) {
        if (!this->table_seen_this_session_)
          ESP_LOGW(TAG, "heat-pump boot push seen (full settings table incl. 1780-1798)");
        this->table_seen_this_session_ = true;
        this->responder_.note_table_complete();
        this->last_table_push_ms_ = millis();
      }
    } else {
      this->a_w_bad_++;
      why = "malformed FC10";
    }
  } else if (fc == 0x03) {
    if (n == 8) {
      this->a_r_req_++;
      this->a_rq_qty_ = (uint16_t) ((d[4] << 8) | d[5]);
    } else if (n >= 5 && n == (size_t) d[2] + 5) {
      gw_reply = true;
      if (this->a_rq_qty_ && d[2] == this->a_rq_qty_ * 2) {
        this->a_r_ok_++;
      } else {
        this->a_r_unmatched_++;
        why = "FC03 reply with no matching request";
      }
      this->a_rq_qty_ = 0;
    } else {
      this->a_r_bad_++;
      why = "malformed FC03";
    }
  } else if (fc == 0x06 && n == 8) {
    // Never used by the heat pump in any capture; counted in case it ever appears.
    // Request and reply are byte-identical for FC06, so this is a raw sighting count.
    this->a_06_++;
    why = "FC06 (write single register) - not normally used on this bus";
  } else if (fc & 0x80) {
    this->a_exc_++;
    why = "ERROR/exception reply";
  } else if (fc == 0x17) {
    this->a_17_++;
    this->audit_handshake_(d, n, gw_reply, why);
  } else {
    this->a_other_fc_++;
    why = "unhandled function code";
  }
  if (gw_reply) {
    this->last_gw_reply_ms_ = millis();
    this->has_gw_reply_ = true;
  }
  if (why && this->a_logged_ < 30) {
    this->a_logged_++;
    ESP_LOGW(TAG_AUDIT, "%s, len=%u: %s", why, (unsigned) n, format_hex_pretty(d, n).c_str());
  }
}

// ---------------------------------------------------------------------------------------
// Handshake check: compute the CM930 answer for every challenge and compare it with a real
// gateway's reply. The computed answer only goes to the log; the ESP never answers FC23.
// ---------------------------------------------------------------------------------------
static std::string hex16_(const uint8_t *p) { return format_hex(p, 16); }

void ThermiaSniffer::audit_handshake_(const uint8_t *d, size_t n, bool &gw_reply, const char *&why) {
  using Ev = cm930::HandshakeChecker::Event;
  const uint32_t now = millis();
  switch (this->hs_.on_frame(d, n)) {
    case Ev::REQUEST:
      if (this->hs_.prev_unanswered() && this->hs_last_ == HsResult::PENDING)
        ESP_LOGW(TAG_AUDIT, "HANDSHAKE: the previous request got NO reply");
      this->hs_last_ = HsResult::PENDING;
      this->hs_req_ms_ = now;
      ESP_LOGW(TAG_AUDIT, "HANDSHAKE #%u request: write %u / read %u%s  challenge: %s", (unsigned) this->hs_.requests(),
               (unsigned) this->hs_.write_start(), (unsigned) this->hs_.read_start(),
               this->hs_.canonical() ? "" : " (NOT the usual 1820/1840!)", hex16_(this->hs_.challenge()).c_str());
      ESP_LOGW(TAG_AUDIT, "HANDSHAKE #%u computed answer (not sent): %s", (unsigned) this->hs_.requests(),
               hex16_(this->hs_.expected()).c_str());
      break;
    case Ev::REPLY_MATCH:
      gw_reply = true;
      this->hs_last_ = HsResult::MATCH;
      this->hs_last_reply_ms_ = now - this->hs_req_ms_;
      ESP_LOGW(TAG_AUDIT, "HANDSHAKE reply: %s  -> MATCHES computed answer (%u of %u checked, reply after ~%u ms)",
               hex16_(this->hs_.got()).c_str(), (unsigned) this->hs_.matches(),
               (unsigned) (this->hs_.matches() + this->hs_.mismatches()), (unsigned) this->hs_last_reply_ms_);
      break;
    case Ev::REPLY_MISMATCH:
      gw_reply = true;
      this->hs_last_ = HsResult::MISMATCH;
      this->hs_last_reply_ms_ = now - this->hs_req_ms_;
      ESP_LOGE(TAG_AUDIT, "HANDSHAKE reply: %s  -> MISMATCH, computed %s (reply after ~%u ms)",
               hex16_(this->hs_.got()).c_str(), hex16_(this->hs_.expected()).c_str(),
               (unsigned) this->hs_last_reply_ms_);
      break;
    case Ev::REPLY_UNEXPECTED:
      gw_reply = true;
      why = "FC17 reply with no handshake request we could compute";
      break;
    case Ev::MALFORMED:
      why = "FC17 in an unrecognised shape (not 8 regs / 16 bytes)";
      break;
    case Ev::NONE:
      break;
  }
}

void ThermiaSniffer::handshake_timeout_check_() {
  if (this->hs_last_ != HsResult::PENDING || !this->hs_.pending()) return;
  if (millis() - this->hs_req_ms_ < HS_REPLY_TIMEOUT_MS) return;
  this->hs_last_ = HsResult::NO_REPLY;
  ESP_LOGW(TAG_AUDIT, "HANDSHAKE #%u: no gateway reply within %u ms (computed answer was %s, not sent)",
           (unsigned) this->hs_.requests(), (unsigned) HS_REPLY_TIMEOUT_MS, hex16_(this->hs_.expected()).c_str());
}

std::string ThermiaSniffer::handshake_status() const {
  const unsigned checked = this->hs_.matches() + this->hs_.mismatches();
  switch (this->hs_last_) {
    case HsResult::NONE:
      return "none seen since ESP boot";
    case HsResult::PENDING:
      return "waiting for gateway reply";
    case HsResult::MATCH:
      return str_sprintf("MATCH (%u of %u checked, reply ~%u ms)", (unsigned) this->hs_.matches(), checked,
                         (unsigned) this->hs_last_reply_ms_);
    case HsResult::MISMATCH:
      return str_sprintf("MISMATCH (%u of %u checked) - see log", (unsigned) this->hs_.mismatches(), checked);
    case HsResult::NO_REPLY:
      return str_sprintf("no gateway reply (answer computed, %u matched so far)", (unsigned) this->hs_.matches());
  }
  return "unknown";
}

// ---------------------------------------------------------------------------------------
// Alarm bits 2143..2147
// ---------------------------------------------------------------------------------------
bool ThermiaSniffer::alarms_known() const {
  return this->store_.has(2143) || this->store_.has(2144) || this->store_.has(2145);
}

bool ThermiaSniffer::any_alarm_bit() const {
  for (uint16_t r = alarm_bits::FIRST_REG; r <= alarm_bits::LAST_REG; r++)
    if (this->store_.has(r) && this->store_.get(r) != 0) return true;
  return false;
}

std::string ThermiaSniffer::active_alarms() const {
  std::string out;
  size_t omitted = 0;
  for (uint16_t r = alarm_bits::FIRST_REG; r <= alarm_bits::LAST_REG; r++)
    if (this->store_.has(r)) alarm_bits::describe(r, this->store_.get(r), out, 235, omitted);
  if (out.empty()) return "None";
  if (omitted) out += str_sprintf(" +%u more", (unsigned) omitted);
  return out;
}

// ---------------------------------------------------------------------------------------
// Gateway shadow: compare the reply the ESP would send with a real gateway's (only while one
// is connected). ACK-MISMATCH is a model bug; READ_CHANGE is a cloud change (expected,
// informational); MAILBOX_EVENT is a mailbox flag the real gateway raised.
// ---------------------------------------------------------------------------------------
void ThermiaSniffer::audit_shadow_(const uint8_t *d, size_t n) {
  using K = cm930::GatewayShadow::Kind;
  auto r = this->shadow_.on_frame(d, n);
  switch (r.kind) {
    case K::NONE:
    case K::OK:
      break;
    case K::ACK_MISMATCH:
      this->shadow_last_ = str_sprintf("ACK-MISMATCH at %u x%u: %s", (unsigned) r.start, (unsigned) r.qty,
                                       r.detail.c_str());
      ESP_LOGE(TAG_AUDIT, "SHADOW %s  <-- model bug (acks are fully determined)", this->shadow_last_.c_str());
      break;
    case K::READ_CHANGE:
      // Not a bug: the gateway served values that differ from what we had (a cloud change,
      // or a register we never saw written). Logged at INFO so a day of running is readable.
      ESP_LOGI(TAG_AUDIT, "SHADOW read-change at %u x%u: %s", (unsigned) r.start, (unsigned) r.qty, r.detail.c_str());
      break;
    case K::MAILBOX_EVENT:
      ESP_LOGW(TAG_AUDIT, "SHADOW mailbox event (1800): %s  <-- gateway raised a re-read flag", r.detail.c_str());
      break;
    case K::READ_FAIL:
      this->shadow_last_ = str_sprintf("read-fail at x%u: %s", (unsigned) r.qty, r.detail.c_str());
      ESP_LOGW(TAG_AUDIT, "SHADOW %s", this->shadow_last_.c_str());
      break;
  }
}

std::string ThermiaSniffer::shadow_status() const {
  const unsigned bugs = this->shadow_.ack_mismatch() + this->shadow_.read_fail();
  if (this->shadow_.replies_checked() == 0) return "nothing checked yet";
  if (bugs == 0)
    return str_sprintf("OK (%u replies: %u acks, %u reads; %u cloud changes, %u mailbox events)",
                       (unsigned) this->shadow_.replies_checked(), (unsigned) this->shadow_.acks_ok(),
                       (unsigned) this->shadow_.reads_ok(), (unsigned) this->shadow_.read_change(),
                       (unsigned) this->shadow_.mailbox_event());
  return str_sprintf("MISMATCH x%u - %s", bugs, this->shadow_last_.c_str());
}

float ThermiaSniffer::seconds_since_hp_update() const {
  if (!this->hp_update_seen_) return NAN;
  return (float) (millis() - this->last_hp_update_ms_) / 1000.0f;
}

// Request a full resync: serve the resync mailbox pattern so the heat pump re-pushes its
// settings table (1000-1777). Armed only.
void ThermiaSniffer::request_full_resync() {
  if (!this->armed_) {
    this->last_write_result_ = "resync refused: not armed";
    ESP_LOGW(TAG, "full resync refused: not armed");
    return;
  }
  this->responder_.request_resync(millis(), 35000);  // 35 s wall-clock hold, auto-reverts
  this->last_write_result_ = "full resync requested (heat pump will re-push the table)";
  ESP_LOGW(TAG, "full resync REQUESTED: serving resync mailbox; heat pump should re-push 1000-1777");
}


// Write one settings register (1000-1349 lower blocks) via its 1801 re-read flag.
void ThermiaSniffer::set_writable_reg(uint16_t reg, float value, int lo, int hi, uint16_t flag_value,
                                      const char *label) {
  // Negative-range registers are refused here: their bus encoding differs per register (1077
  // is two's complement, 1070/1111 store a positive magnitude). They must use
  // set_writable_reg_signed() with the encoding that has been seen on the bus for them.
  if (lo < 0) {
    this->last_write_result_ = str_sprintf("refused: reg %u has a negative range, use the signed path (%s)",
                                           (unsigned) reg, label);
    ESP_LOGW(TAG, "%s (reg %u) write refused: negative range needs set_writable_reg_signed", label, (unsigned) reg);
    return;
  }
  int v = (int) lroundf(value);
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  this->stage_reg_write_(reg, (uint16_t) v, v, 1 /*1801*/, flag_value, label);
}

// Write for a negative-range register whose bus encoding has been confirmed by reading it.
// value/lo/hi are in display units (e.g. -20..-1); the bus word is produced by `enc`.
void ThermiaSniffer::set_writable_reg_signed(uint16_t reg, float value, int lo, int hi, uint16_t flag_value,
                                             const char *label, thermia::Encoding enc) {
  int v = (int) lroundf(value);
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  this->stage_reg_write_(reg, thermia::encode_value(v, enc), v, 1 /*1801*/, flag_value, label);
}

// Write for an UPPER settings register (1350-1798), whose blocks are selected by mailbox word
// 1800 (W0) instead of 1801. Only W0 bit 0 (1800=1 -> block 1350-1369) is confirmed on our bus.
void ThermiaSniffer::set_writable_reg_upper(uint16_t reg, float value, int lo, int hi, uint16_t flag_value,
                                            const char *label) {
  if (lo < 0) {
    this->last_write_result_ = str_sprintf("refused: reg %u negative range not supported here (%s)",
                                           (unsigned) reg, label);
    return;
  }
  int v = (int) lroundf(value);
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  this->stage_reg_write_(reg, (uint16_t) v, v, 0 /*1800*/, flag_value, label);
}

// Shared gate + staging for every generic write. `raw` goes on the bus; `shown` is for the log.
void ThermiaSniffer::stage_reg_write_(uint16_t reg, uint16_t raw, int shown, uint16_t flag_idx,
                                      uint16_t flag_value, const char *label) {
  const unsigned flag_reg = 1800u + flag_idx;
  if (!this->armed_) {
    this->last_write_result_ = str_sprintf("refused: not armed (%s)", label);
    ESP_LOGW(TAG, "%s (reg %u) write refused: not armed", label, (unsigned) reg);
    return;
  }
  if (!this->responder_.has(reg)) {
    this->last_write_result_ = str_sprintf("refused: reg %u not seen this session (%s)", (unsigned) reg, label);
    ESP_LOGW(TAG, "%s (reg %u) write refused: register not seen this session", label, (unsigned) reg);
    return;
  }
  const uint16_t before = this->responder_.get(reg);
  if (!this->responder_.stage_write(reg, raw, flag_idx, flag_value, millis(), 15000)) {
    this->last_write_result_ = str_sprintf("refused: reg %u unseen", (unsigned) reg);
    return;
  }
  this->mark_pending_readback_(reg, raw);
  this->last_write_result_ = str_sprintf("%s (reg %u): raw %u -> %d (bus 0x%04X, flag %u=%u)", label,
                                         (unsigned) reg, (unsigned) before, shown, (unsigned) raw, flag_reg,
                                         (unsigned) flag_value);
  ESP_LOGW(TAG, "%s (reg %u): raw %u -> %d (bus 0x%04X); raised %u=%u", label, (unsigned) reg,
           (unsigned) before, shown, (unsigned) raw, flag_reg, (unsigned) flag_value);
}

// Signed-aware: uses the register's `signed:` setting from the YAML (unconfigured registers
// read as unsigned). Raw bus value - YAML sensor filters (scale/sign) are NOT applied here.
float ThermiaSniffer::get_reg_value(uint16_t reg) const {
  if (!this->responder_.has(reg)) return NAN;
  return thermia::decode_value(this->responder_.get(reg), this->reg_is_signed_(reg));
}

bool ThermiaSniffer::reg_is_signed_(uint16_t reg) const {
  for (const auto &s : this->sensors_)
    if (s.reg == reg) return s.is_signed;
  return false;
}

// =======================================================================================
// Takeover: respond AS the gateway. This is the only code in the project that transmits;
// every gate below must pass before a single byte goes out.
// =======================================================================================

// Auto-takeover: decide once per boot whether to self-arm, then seed the store once.
void ThermiaSniffer::auto_takeover_tick_() {
  if (this->manual_disarm_latch_) return;  // user turned it off; stay off until reboot
  if (!this->auto_takeover_ || this->auto_arm_done_) {
    // After the decision: if armed and the store has not been seeded yet, request one resync.
    if (this->armed_ && this->auto_takeover_ && !this->auto_resync_done_ &&
        !this->table_seen_this_session_ && !this->responder_.resync_active() &&
        this->gw_addressed_seen_ &&                       // HP is present and polling us
        millis() - this->boot_ms_ > this->auto_listen_ms_) {  // past the gateway-listen window
      this->request_full_resync();
      this->auto_resync_done_ = true;
      ESP_LOGW(TAG, "auto-takeover: armed and HP present - auto-requested a full resync to seed values");
    }
    return;
  }
  // Still within the listen window: wait.
  if (millis() - this->boot_ms_ < this->auto_listen_ms_) {
    // If we hear the real gateway at any point in the window, abandon auto-arm this boot.
    if (this->gateway_replying()) {
      this->auto_arm_done_ = true;
      ESP_LOGW(TAG, "auto-takeover: real gateway heard during listen window - NOT self-arming");
    }
    return;
  }
  // Window elapsed. Decide.
  this->auto_arm_done_ = true;
  if (this->gateway_replying()) {
    ESP_LOGW(TAG, "auto-takeover: real gateway present - staying disarmed");
    return;
  }
  ESP_LOGW(TAG, "auto-takeover: no real gateway heard in %u s - self-arming",
           (unsigned) (this->auto_listen_ms_ / 1000));
  this->try_arm_();  // same gates as a manual arm; refuses if something is wrong
}

// Arm only if EVERY precondition holds. Called from the HA switch via request_takeover.
void ThermiaSniffer::try_arm_() {
  this->arm_refused_ = nullptr;
  if (!this->takeover_capable_) { this->arm_refused_ = "not takeover_capable (YAML)"; }
  else if (this->de_pin_ == nullptr) { this->arm_refused_ = "no flow_control_pin configured"; }
  else if (this->gateway_replying()) { this->arm_refused_ = "real gateway still replying - unplug it first"; }
  if (this->arm_refused_ != nullptr) {
    ESP_LOGE(TAG, "takeover ARM REFUSED: %s", this->arm_refused_);
    this->armed_ = false;
    return;
  }
  this->armed_ = true;
  this->disarm_reason_ = nullptr;
  ESP_LOGW(TAG, "takeover ARMED: answering 0x%02X as the gateway%s", this->decoder_.addr(),
           this->table_seen_this_session_ ? "" : " (store not seeded yet: a resync loads it)");
}

void ThermiaSniffer::request_takeover(bool on) {
  if (on) {
    this->manual_disarm_latch_ = false;  // user re-armed: let auto-takeover work again too
    this->try_arm_();
  } else {
    if (this->armed_) ESP_LOGW(TAG, "takeover DISARMED by request (stays off until reboot)");
    this->armed_ = false;
    // Latch a MANUAL disarm so auto-takeover won't re-arm - BUT ignore the call that the
    // Takeover Arm switch makes at boot when it restores to OFF, which would otherwise veto
    // auto-takeover on every boot. The boot-restore call lands in the first moment after boot;
    // a real user toggle comes later. A time-based grace is safe regardless of whether this
    // runs before or after the component's config setters.
    const bool boot_grace = (millis() - this->boot_ms_) < 4000;
    if (!boot_grace) {
      this->manual_disarm_latch_ = true;  // sticks until reboot
      this->disarm_reason_ = "manually disarmed (until reboot)";
    } else {
      this->disarm_reason_ = "disarmed (boot default; auto-takeover may still arm)";
    }
    if (this->de_pin_ != nullptr) this->de_pin_->digital_write(false);
  }
}

void ThermiaSniffer::force_disarm_(const char *why) {
  if (this->armed_) ESP_LOGE(TAG, "takeover AUTO-DISARMED: %s", why);
  this->armed_ = false;
  this->disarm_reason_ = why;
  if (this->de_pin_ != nullptr) this->de_pin_->digital_write(false);
}

// Decide whether to answer this frame, build the reply, and (only if safe) transmit it.
void ThermiaSniffer::maybe_respond_(const uint8_t *d, size_t n) {
  // Track the real gateway independently: if we ever hear IT reply while armed, something
  // is wrong (two gateways on the bus) - disarm immediately.
  if (this->armed_ && this->has_gw_reply_ && (millis() - this->last_gw_reply_ms_) < 50) {
    this->force_disarm_("heard the real gateway reply while armed (two gateways on the bus)");
    return;
  }
  if (!this->armed_ || this->de_pin_ == nullptr) return;
  if (n < 4 || d[0] != this->decoder_.addr()) return;  // only frames addressed to the gateway

  // Count mailbox read REQUESTS (FC03 to 1800 x6), to measure how often we actually serve them.
  const bool is_mbx_req = (d[1] == 0x03 && n == 8 &&
                           (uint16_t) ((d[2] << 8) | d[3]) == cm930::GatewayResponder::MAILBOX_START &&
                           (uint16_t) ((d[4] << 8) | d[5]) == cm930::GatewayResponder::MAILBOX_QTY);
  if (is_mbx_req) this->mbx_req_++;

  auto reply = this->responder_.build_reply(d, n);
  if (!reply.has_reply()) return;  // nothing to send (incl. an unseen read we suppress)

  // Final bus-idle gate: the request frame we just parsed was the last thing on the bus,
  // and the framer emitted it after a >=gap idle. Require the bus still idle right now.
  const uint32_t now_us = micros();
  if (this->framer_.idle_us(now_us) < 1500) {  // someone is mid-frame; skip this one
    if (is_mbx_req) this->mbx_dropped_++;
    return;
  }

  this->send_frame_(reply.bytes, reply.len);
  if (reply.was_mailbox) this->mbx_served_++;
  if (reply.was_mailbox) this->responder_.after_mailbox_sent();  // advance a write-flag pulse
}


void ThermiaSniffer::send_frame_(const uint8_t *b, size_t len) {
  // Drive RS485 to transmit, write, wait for the UART to drain, release the driver.
  // NOTE: flush() blocks until all bytes are clocked out. At 9600 baud a large block reply
  // (~47 bytes) takes ~49 ms on the wire, so ESPHome may log a one-off "took a long time
  // (~56 ms)" warning on those frames. This is expected and harmless - it is the wire time,
  // not a stall, and captures show bad=0 (no missed/corrupt frames) even under heavy load.
  // The driver MUST stay enabled until the last byte leaves, so this wait cannot be skipped
  // without risking a corrupted frame; a non-blocking release was judged not worth that risk.
  this->de_pin_->digital_write(true);
  delayMicroseconds(50);               // let the transceiver settle into TX
  this->write_array(b, len);
  this->flush();                        // block until all bytes are clocked out
  delayMicroseconds(100);               // hold past the last stop bit
  this->de_pin_->digital_write(false);  // back to receive
  this->tx_sent_++;
  ESP_LOGD(TAG_AUDIT, "TX %u bytes: %s", (unsigned) len, format_hex_pretty(b, len).c_str());
}

std::string ThermiaSniffer::takeover_status() const {
  if (!this->takeover_capable_) return "not capable (YAML)";
  // Kept free of counters so it only changes on a real state change (HA activity log).
  // Frame counts and the last write have their own sensors.
  if (this->armed_) return "ARMED - answering as gateway";
  if (this->disarm_reason_) return str_sprintf("disarmed: %s", this->disarm_reason_);
  if (this->arm_refused_) return str_sprintf("not armed: %s", this->arm_refused_);
  return "ready to arm (disarmed)";
}

bool ThermiaSniffer::gateway_replying() const {
  return this->has_gw_reply_ && (millis() - this->last_gw_reply_ms_) < 15000;
}

void ThermiaSniffer::dump_config() {
  ESP_LOGCONFIG(TAG, "Thermia gateway replacement");
  ESP_LOGCONFIG(TAG, "  Gateway address: 0x%02X", this->decoder_.addr());
  ESP_LOGCONFIG(TAG, "  Takeover: %s; starts DISARMED, transmits only when armed",
                this->takeover_capable_ ? "CAPABLE" : "disabled (receive-only)");
  ESP_LOGCONFIG(TAG, "  Auto-takeover: %s%s", this->auto_takeover_ ? "ON" : "off",
                this->auto_takeover_ ? str_sprintf(" - self-arms after %u s with no real gateway, then one resync",
                                                   (unsigned) (this->auto_listen_ms_ / 1000)).c_str()
                                     : "");
  ESP_LOGCONFIG(TAG, "  Mapped registers: %u", (unsigned) this->sensors_.size());
  ESP_LOGCONFIG(TAG, "  Log every change: %s", YESNO(this->log_changes_));
  ESP_LOGCONFIG(TAG, "  Live I/O decoding (FC23 to other devices): fresh for %u ms", (unsigned) LIVE_MAX_AGE_MS);
  ESP_LOGCONFIG(TAG, "  Handshake check and gateway shadow: active only while a real gateway is connected");
}

}  // namespace thermia_sniffer
}  // namespace esphome
