#pragma once
// registers_json.h  v2026.10.20.3  JSON snapshot of every register the ESP holds
//
// Builds the body served at GET /registers.json. Pure C++ (no ESPHome), so it is host-tested.
// Values are RAW unsigned 16-bit, exactly as on the bus; signed/scaled meaning is left to the
// reader. Only registers seen since boot are included (no defaults, no invented values).
//
// {"uptime_s":123,"armed":true,
//  "reg":{"1000":22,"1001":20,...},                  <- every register seen (gateway 0x0F)
//  "live":{"dev02.w.10":5,"dev02.r.10":7,...}}       <- fresh FC23 live I/O values
//
// Live keys: dev<device hex>.<w = heat pump wrote | r = device replied>.<address decimal>.
// Only live values updated recently (the caller filters) are included, so a device that
// stops talking drops out of the JSON rather than freezing at its last value.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "register_store.h"

namespace thermia {

struct LiveEntry {
  bool reply;     // false: heat pump wrote to the device; true: device replied
  uint8_t dev;    // Modbus address of the device (0x02, 0x04, ...)
  uint16_t addr;  // register address within that device
  uint16_t value;
};

inline std::string build_registers_json(const RegisterStore &store, const std::vector<LiveEntry> &live,
                                        uint32_t uptime_s, bool armed) {
  std::string out;
  out.reserve(14 * store.known_count() + 22 * live.size() + 64);
  char buf[40];
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
  out += "}}";
  return out;
}

}  // namespace thermia
