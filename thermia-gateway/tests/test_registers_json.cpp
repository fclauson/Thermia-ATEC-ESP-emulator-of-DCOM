// Host-only test (never copy into the component folder): /registers.json body.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_gateway test_registers_json.cpp -o t && ./t
#include <cstdio>
#include "registers_json.h"

static int fails = 0;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%-4s %s\n", ok ? "ok" : "FAIL", what); }

int main() {
  thermia::RegisterStore store;
  check("empty store -> empty objects",
        thermia::build_registers_json(store, {}, 5, false) == "{\"uptime_s\":5,\"armed\":false,\"reg\":{},\"live\":{}}");
  store.update(1000, 22);
  store.update(1077, 0xFFEC);   // -20 on the wire: must stay RAW unsigned
  store.update(2160, 21975);
  std::vector<thermia::LiveEntry> live = {{false, 0x02, 10, 5}, {true, 0x04, 3, 65535}};
  std::string j = thermia::build_registers_json(store, live, 3600, true);
  printf("     %s\n", j.c_str());
  check("raw unsigned for negative register", j.find("\"1077\":65516") != std::string::npos);
  check("registers in order", j.find("\"1000\":22,\"1077\":65516,\"2160\":21975") != std::string::npos);
  check("live keys", j.find("\"dev02.w.10\":5,\"dev04.r.3\":65535") != std::string::npos);
  check("armed + uptime", j.rfind("{\"uptime_s\":3600,\"armed\":true,", 0) == 0);
  // Full-size store: every register 1000..2239 seen, check it stays well-formed and its size
  for (int r = 1000; r < 2240; r++) store.update(r, 65535);
  std::string big = thermia::build_registers_json(store, live, 1, true);
  check("full store ends with }}", big.size() > 2 && big.substr(big.size() - 2) == "}}");
  printf("     full-store size: %zu bytes\n", big.size());
  printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
