#pragma once
// registers_json.h  v2026.10.22.1  JSON snapshot of every register the ESP holds
//
// Builds the body served at GET /registers.json. Pure C++ (no ESPHome), so it is host-tested.
// Values are RAW unsigned 16-bit, exactly as on the bus; signed/scaled meaning is left to the
// reader. Only registers seen since boot are included (no defaults, no invented values).
//
// {"uptime_s":123,"armed":true,
//  "reg":{"1000":22,"1001":20,...},                  <- every register seen (gateway 0x0F)
//  "live":{"dev02.w.10":5,"dev02.r.10":7,...},      <- fresh FC23 live I/O values
//  "uptime_ms":3600123,
//  "events":{"last_seq":57,"list":[[56,3590001,"2149",16],[57,3591200,"dev04.w.44020",78]]}}
//
// events: the watchlist changes still held in the ring (bus_events.h), oldest first, as
// [seq, ESP uptime ms when it happened, key, raw value]. A reader converts a time with
// event_time = fetch_time - (uptime_ms - ms) / 1000; uptime_ms is when this JSON was built.
//
// Live keys: dev<device hex>.<w = heat pump wrote | r = device replied>.<address decimal>.
// Only live values updated recently (the caller filters) are included, so a device that
// stops talking drops out of the JSON rather than freezing at its last value.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "register_store.h"
#include "bus_events.h"

namespace thermia {

struct LiveEntry {
  bool reply;     // false: heat pump wrote to the device; true: device replied
  uint8_t dev;    // Modbus address of the device (0x02, 0x04, ...)
  uint16_t addr;  // register address within that device
  uint16_t value;
};

inline std::string build_registers_json(const RegisterStore &store, const std::vector<LiveEntry> &live,
                                        uint32_t uptime_s, bool armed, const EventRing *events = nullptr,
                                        uint32_t now_ms = 0) {
  std::string out;
  out.reserve(14 * store.known_count() + 22 * live.size() + (events ? 34u * events->count() : 0u) + 96);
  char buf[96];  // fits the longest fragment (the events header, ~70 chars)
  snprintf(buf, sizeof(buf), "{\"uptime_s\":%u,\"armed\":%s,\"reg\":{", (unsigned) uptime_s,
           armed ? "true" : "false");
  out += buf;
  bool first = true;
  for (uint16_t r = 0; r < RegisterStore::REG_MAX; r++) {
    if (!store.has(r)) continue;
    snprintf(buf, sizeof(buf), "%s\"%u\":%u", first ? "" : ",", (unsigned) r, (unsigned) store.get(r));
    out += buf;
    first = false;
  }
  out += "},\"live\":{";
  first = true;
  for (const auto &e : live) {
    snprintf(buf, sizeof(buf), "%s\"dev%02X.%c.%u\":%u", first ? "" : ",", (unsigned) e.dev, e.reply ? 'r' : 'w',
             (unsigned) e.addr, (unsigned) e.value);
    out += buf;
    first = false;
  }
  out += "}";
  if (events != nullptr) {
    snprintf(buf, sizeof(buf), ",\"uptime_ms\":%u,\"events\":{\"last_seq\":%u,\"list\":[", (unsigned) now_ms,
             (unsigned) events->last_seq());
    out += buf;
    for (uint16_t i = 0; i < events->count(); i++) {
      const BusEvent &e = events->at(i);
      snprintf(buf, sizeof(buf), "%s[%u,%u,\"", i ? "," : "", (unsigned) e.seq, (unsigned) e.ms);
      out += buf;
      out += event_key(e);
      snprintf(buf, sizeof(buf), "\",%u]", (unsigned) e.value);
      out += buf;
    }
    out += "]}";
  }
  out += "}";
  return out;
}

}  // namespace thermia
