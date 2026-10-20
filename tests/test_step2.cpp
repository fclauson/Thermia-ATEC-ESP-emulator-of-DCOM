// HOST-ONLY: keep in tests/, NOT in components/thermia_sniffer/ (it has its own main()).
// Step 2: gateway shadow -- predicted reply vs the real gateway, replayed from captures.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer test_step2.cpp -o t
#include "modbus_sniff.h"
#include "gateway_shadow.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t) strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

struct Out { uint32_t checked, ok, ack_mm, read_chg, mbx, read_fail, frames; };

static Out replay(const std::string &path, bool verbose) {
  cm930::GatewayShadow sh(0x0F);
  modbus_sniff::Framer fr(3000, 30000);
  std::ifstream f(path);
  std::string line;
  uint32_t last_end = 0, frames = 0;
  auto drain = [&](uint32_t now) {
    const uint8_t *d; size_t n;
    while (fr.poll(now, &d, &n)) {
      frames++;
      auto r = sh.on_frame(d, n);
      using K = cm930::GatewayShadow::Kind;
      if (verbose && r.kind != K::NONE && r.kind != K::OK) {
        const char *kn = r.kind == K::ACK_MISMATCH ? "ACK-MISMATCH" : r.kind == K::READ_CHANGE ? "read-change"
                        : r.kind == K::MAILBOX_EVENT ? "mailbox-event" : "read-fail";
        printf("    %-13s %u x%u  %s\n", kn, r.start, r.qty, r.detail.c_str());
      }
    }
  };
  while (std::getline(f, line)) {
    std::istringstream is(line);
    double t; std::string h;
    if (!(is >> t >> h)) continue;
    auto b = unhex(h);
    uint32_t us = (uint32_t) (t * 1e6);
    if (us < last_end + 4000) us = last_end + 4000;
    drain(us);
    for (size_t i = 0; i < b.size(); i++) fr.push(b[i], us + (uint32_t) (i * 1146));
    last_end = us + (uint32_t) (b.size() * 1146);
    drain(last_end + 3500);
  }
  return {sh.replies_checked(), sh.ok(), sh.ack_mismatch(), sh.read_change(), sh.mailbox_event(), sh.read_fail(), frames};
}

int main() {
  const std::string dir = "../captures/";
  const char *files[] = {
      "thermia_capture_20261004_134926_-_cold_start_up.log",
      "thermia_capture_20260924_210001_on_line_change_-_reflected_down_to_HP.log",
      "thermia_capture_20260924_210001_at_HP_change_of_value_and_it_appears_on_line_.log",
      "thermia_capture_20261004_160920_-_cold_start_-_no_dcm.log",
      "thermia_capture_20261004_162316c_cold_start_-_wait_-_plug_gateway_in.log",
  };
  for (auto fn : files) {
    printf("%s\n", fn);
    Out o = replay(dir + fn, true);
    printf("  frames=%u checked=%u OK=%u | ACK-MM=%u read-change=%u mailbox=%u read-fail=%u\n",
           (unsigned) o.frames, (unsigned) o.checked, (unsigned) o.ok, (unsigned) o.ack_mm, (unsigned) o.read_chg,
           (unsigned) o.mbx, (unsigned) o.read_fail);
    // The hard requirement: the model never gets an ACK wrong, in any capture.
    CHECK(o.ack_mm == 0);
    // Every reply we checked was either OK or a recorded change/event, never an unexplained fail.
    CHECK(o.read_fail == 0);
  }

  // Targeted check: the online-change capture must show reg 1000 changing on a 1000x13 read.
  {
    printf("online-change detail (expect a 1000-block read-change)\n");
    Out o = replay(dir + "thermia_capture_20260924_210001_on_line_change_-_reflected_down_to_HP.log", false);
    CHECK(o.read_chg >= 1);
  }

  // Unit: a hand-built FC16 write then an 8-byte ack must be OK; a read of the written
  // block must be OK; a read returning a changed value must be READ_CHANGE once then OK.
  {
    printf("unit: write / ack / read / change\n");
    cm930::GatewayShadow sh(0x0F);
    auto fix = [](std::vector<uint8_t> f) { uint16_t c = modbus_sniff::crc16(f.data(), f.size()); f.push_back(c & 0xFF); f.push_back(c >> 8); return f; };
    using K = cm930::GatewayShadow::Kind;
    // write 1000..1001 = 22,20
    auto wr = fix({0x0F, 0x10, 0x03, 0xE8, 0x00, 0x02, 0x04, 0x00, 0x16, 0x00, 0x14});
    CHECK(sh.on_frame(wr.data(), wr.size()).kind == K::NONE);
    auto ack = fix({0x0F, 0x10, 0x03, 0xE8, 0x00, 0x02});
    CHECK(sh.on_frame(ack.data(), ack.size()).kind == K::OK);
    // read request 1000x2, reply with the same values -> OK
    auto rq = fix({0x0F, 0x03, 0x03, 0xE8, 0x00, 0x02});
    sh.on_frame(rq.data(), rq.size());
    auto rp = fix({0x0F, 0x03, 0x04, 0x00, 0x16, 0x00, 0x14});
    CHECK(sh.on_frame(rp.data(), rp.size()).kind == K::OK);
    // read again, reply shows 1000 changed 22->23 -> READ_CHANGE, then OK on repeat
    sh.on_frame(rq.data(), rq.size());
    auto rp2 = fix({0x0F, 0x03, 0x04, 0x00, 0x17, 0x00, 0x14});
    auto r = sh.on_frame(rp2.data(), rp2.size());
    CHECK(r.kind == K::READ_CHANGE && r.detail == "1000:22->23");
    sh.on_frame(rq.data(), rq.size());
    CHECK(sh.on_frame(rp2.data(), rp2.size()).kind == K::OK);
    CHECK(sh.get(1000) == 23 && sh.has(1000));
    // mailbox read with a raised flag -> MAILBOX_EVENT
    auto mrq = fix({0x0F, 0x03, 0x07, 0x08, 0x00, 0x06});
    sh.on_frame(mrq.data(), mrq.size());
    auto mrp = fix({0x0F, 0x03, 0x0C, 0, 0, 0, 1, 0, 0, 0, 0, 0x07, 0x7F, 0, 6});
    CHECK(sh.on_frame(mrp.data(), mrp.size()).kind == K::MAILBOX_EVENT);
  }

  printf(fails ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
  return fails ? 1 : 0;
}
