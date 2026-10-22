// Host-only test (never copy into the component folder): event ring, watchlist, block map,
// request memory and the events part of /registers.json.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_gateway test_bus_events.cpp -o t && ./t
#include <cstdio>
#include "registers_json.h"
using namespace thermia;
static int fails = 0;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%-4s %s\n", ok ? "ok" : "FAIL", what); }

int main() {
  check("watch 2149, 1780, 1798", is_watched_reg(2149) && is_watched_reg(1780) && is_watched_reg(1798));
  check("not watch 2160, 1779, 1799", !is_watched_reg(2160) && !is_watched_reg(1779) && !is_watched_reg(1799));
  check("watch live ABF4/A80C/A811 cmds", is_watched_live(false, 4, 0xABF4) && is_watched_live(false, 2, 0xA80C) &&
                                              is_watched_live(false, 2, 0xA811));
  check("not watch replies or others", !is_watched_live(true, 4, 0xABF4) && !is_watched_live(false, 2, 0xA80F));

  uint8_t w, b;
  check("1000 -> W1 bit 0", settings_block_bit(1000, w, b) && w == 1 && b == 0);
  check("1330 -> W1 bit 15", settings_block_bit(1330, w, b) && w == 1 && b == 15);
  check("1350 -> W0 bit 0", settings_block_bit(1350, w, b) && w == 0 && b == 0);
  check("1403 -> W0 bit 2", settings_block_bit(1403, w, b) && w == 0 && b == 2);
  check("1733 -> W0 bit 12", settings_block_bit(1733, w, b) && w == 0 && b == 12);
  check("1780 -> W0 bit 15", settings_block_bit(1780, w, b) && w == 0 && b == 15);
  check("1404 not a block", !settings_block_bit(1404, w, b));

  const uint16_t steady[6] = {0, 0, 0, 0, 0x077F, 6}, resync[6] = {0, 0, 0x7FFF, 0xFFFF, 0x0080, 7};
  check("steady recognised", mailbox_is_steady(steady) && !mailbox_is_steady(resync));
  RequestMemory rm;
  rm.note_served(steady, 1000);
  check("no request -> UNSOLICITED", rm.why(1, 0, 2000) == nullptr);
  rm.note_served(resync, 5000);
  check("resync -> lower block requested via 1803", rm.why(1, 3, 20000) != nullptr);
  check("resync -> upper bit 14 requested via 1802", rm.why(0, 14, 20000) != nullptr);
  check("resync does NOT request 06F4 (bit 15)", rm.why(0, 15, 20000) == nullptr);
  check("request expires after 60 s", rm.why(1, 3, 5000 + 60001) == nullptr);
  const uint16_t write1801[6] = {0, 4, 0, 0, 0x077F, 6};
  rm.note_served(write1801, 70000);
  check("1801=4 write -> 1040 block re-read flag", rm.why(1, 2, 71000) != nullptr);

  EventRing ring;
  for (int i = 0; i < 300; i++) ring.push(1000 + i, 0, false, 2149, (uint16_t) i);
  check("ring holds 256 newest", ring.count() == 256 && ring.at(0).seq == 45 && ring.at(255).seq == 300);
  ring.push(2000, 4, false, 0xABF4, 78);
  check("live key text", event_key(ring.at(255)) == "dev04.w.44020");

  RegisterStore store;
  store.update(2149, 16);
  EventRing r2;
  r2.push(3590001, 0, false, 2149, 16);
  r2.push(3591200, 4, false, 0xABF4, 78);
  std::string j = build_registers_json(store, {}, 3600, true, &r2, 3600123);
  printf("     %s\n", j.c_str());
  check("events JSON", j.find("\"uptime_ms\":3600123,\"events\":{\"last_seq\":2,\"list\":[[1,3590001,\"2149\",16],"
                              "[2,3591200,\"dev04.w.44020\",78]]}}") != std::string::npos);
  FILE *f = fopen("/tmp/ev.json", "w"); fputs(j.c_str(), f); fclose(f);
  printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
