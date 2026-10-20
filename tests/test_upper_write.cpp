// Host-only test (never copy into the component folder): a write to an UPPER settings register
// (1363 operation mode) raises its flag in mailbox word 1800 (W0), twice, and leaves 1801 alone.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer test_upper_write.cpp -o t && ./t
#include <cstdio>
#include <vector>
#include "gateway_responder.h"

static int fails = 0;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%-4s %s\n", ok ? "ok" : "FAIL", what); }

int main() {
  cm930::GatewayResponder r;
  // Heat pump pushes block 1350 x20 (FC16 to 0x0F), with 1363 = 4 (hot water mode).
  std::vector<uint8_t> f = {0x0F, 0x10, 0x05, 0x46, 0x00, 0x14, 40};
  for (int i = 0; i < 20; i++) { uint16_t v = (1350 + i == 1363) ? 4 : 0; f.push_back(v >> 8); f.push_back(v & 0xFF); }
  uint16_t crc = modbus_sniff::crc16(f.data(), f.size());
  f.push_back(crc & 0xFF); f.push_back(crc >> 8);
  r.observe(f.data(), f.size());
  check("1363 seen after the HP pushed block 1350", r.has(1363) && r.get(1363) == 4);

  check("stage 1363 = 1 (AUTO) with flag 1800=1", r.stage_write(1363, 1, 0, 1, 1000, 15000));
  check("store now holds 1363 = 1", r.get(1363) == 1);
  check("first pulse: 1800 = 1", r.mailbox(0) == 1);
  check("1801 untouched (0)", r.mailbox(1) == 0);
  r.after_mailbox_sent();                       // pulse served
  check("after pulse 1: 1800 cleared", r.mailbox(0) == 0);
  r.after_mailbox_sent(); r.after_mailbox_sent();  // two quiescent polls
  check("second pulse: 1800 = 1 again", r.mailbox(0) == 1);
  r.after_mailbox_sent();
  check("after pulse 2: 1800 cleared, 1801 still 0", r.mailbox(0) == 0 && r.mailbox(1) == 0);
  r.after_mailbox_sent(); r.after_mailbox_sent(); r.after_mailbox_sent();
  check("no third pulse", r.mailbox(0) == 0);
  printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
