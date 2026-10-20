// Host test for step 1: CM930 handshake computation + checker, and alarm bit decoding.
// Replays the real captures through the same Framer/Decoder the ESP uses.
// HOST-ONLY: keep this file in tests/, NOT in components/thermia_sniffer/ -- ESPHome
// compiles every .cpp in the component folder, and this file has its own main().
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer test_step1.cpp -o test_step1
#include "modbus_sniff.h"
#include "cm930_handshake.h"
#include "alarm_bits.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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
static std::string hex(const uint8_t *p, size_t n) {
  std::string s; char b[3];
  for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", p[i]); s += b; }
  return s;
}

struct Result {
  uint32_t req = 0, match = 0, mismatch = 0, unanswered = 0, frames = 0;
  bool pending = false;
  std::map<uint16_t, uint16_t> regs;
};

static Result replay(const std::string &path) {
  Result r;
  modbus_sniff::Framer fr(3000, 30000);
  modbus_sniff::Decoder dec(0x0F);
  cm930::HandshakeChecker hs(0x0F);
  std::ifstream f(path);
  std::string line;
  auto drain = [&](uint32_t now) {
    const uint8_t *d; size_t n;
    while (fr.poll(now, &d, &n)) {
      r.frames++;
      dec.handle(d, n, [&](uint16_t reg, uint16_t v) { r.regs[reg] = v; });
      auto ev = hs.on_frame(d, n);
      using Ev = cm930::HandshakeChecker::Event;
      if (ev == Ev::REQUEST) printf("    challenge %s computed %s\n", hex(hs.challenge(), 16).c_str(), hex(hs.expected(), 16).c_str());
      if (ev == Ev::REPLY_MATCH) printf("    gateway   %s MATCH\n", hex(hs.got(), 16).c_str());
      if (ev == Ev::REPLY_MISMATCH) printf("    gateway   %s MISMATCH\n", hex(hs.got(), 16).c_str());
    }
  };
  uint32_t last_end = 0;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    double t; std::string h;
    if (!(is >> t >> h)) continue;
    auto b = unhex(h);
    uint32_t us = (uint32_t) (t * 1e6);
    if (us < last_end + 4000) us = last_end + 4000;  // keep frames apart if timestamps are coarse
    drain(us);
    for (size_t i = 0; i < b.size(); i++) fr.push(b[i], us + (uint32_t) (i * 1146));  // 11 bits @ 9600
    last_end = us + (uint32_t) (b.size() * 1146);
    drain(last_end + 3500);
  }
  r.req = hs.requests(); r.match = hs.matches(); r.mismatch = hs.mismatches();
  r.unanswered = hs.unanswered(); r.pending = hs.pending();
  return r;
}

int main() {
  // 1. Reference vector from CM930_HANDSHAKE_071C_0730.md
  {
    printf("vector from CM930_HANDSHAKE_071C_0730.md\n");
    auto c = unhex("2ee156572b8b84a0bc7a65c72d56185d");
    uint8_t out[16];
    cm930::handshake_response(c.data(), out);
    CHECK(hex(out, 16) == "20b8f28a236f28fa0339669e2c6f600d");
    // full frame pair through the checker
    cm930::HandshakeChecker hs;
    auto rq = unhex("0f1707300008071c0008102ee156572b8b84a0bc7a65c72d56185d7840");
    auto rp = unhex("0f171020b8f28a236f28fa0339669e2c6f600d57d9");
    CHECK(modbus_sniff::crc_ok(rq.data(), rq.size()) && modbus_sniff::crc_ok(rp.data(), rp.size()));
    CHECK(hs.on_frame(rq.data(), rq.size()) == cm930::HandshakeChecker::Event::REQUEST);
    CHECK(hs.canonical());
    CHECK(hs.on_frame(rp.data(), rp.size()) == cm930::HandshakeChecker::Event::REPLY_MATCH);
    // corrupted reply (CRC fixed up) must be a mismatch
    hs.on_frame(rq.data(), rq.size());
    rp[5] ^= 0x01;
    uint16_t crc = modbus_sniff::crc16(rp.data(), rp.size() - 2);
    rp[rp.size() - 2] = crc & 0xFF; rp[rp.size() - 1] = crc >> 8;
    CHECK(hs.on_frame(rp.data(), rp.size()) == cm930::HandshakeChecker::Event::REPLY_MISMATCH);
    // other-device FC23 (live I/O) must be ignored
    auto io = unhex("0217a7f8000da80c00070e00400000002800050005000c05005cf9");
    CHECK(hs.on_frame(io.data(), io.size()) == cm930::HandshakeChecker::Event::NONE);
  }

  // 2. Real captures
  const std::string dir = "../captures/";
  struct Case { const char *file; uint32_t req, match, unanswered; bool pending; };
  const Case cases[] = {
      {"thermia_capture_20261004_134926_-_cold_start_up.log", 1, 1, 0, false},
      {"thermia_capture_20261004_160920_-_cold_start_-_no_dcm.log", 6, 0, 5, true},
      {"thermia_capture_20261004_162316c_cold_start_-_wait_-_plug_gateway_in.log", 6, 0, 5, true},
      {"thermia_capture_20260924_210001_at_HP_change_of_value_and_it_appears_on_line_.log", 0, 0, 0, false},
      {"thermia_capture_20260924_210001_on_line_change_-_reflected_down_to_HP.log", 0, 0, 0, false},
  };
  for (const auto &c : cases) {
    printf("%s\n", c.file);
    Result r = replay(dir + c.file);
    printf("  frames=%u requests=%u match=%u mismatch=%u unanswered=%u pending=%d\n", (unsigned) r.frames,
           (unsigned) r.req, (unsigned) r.match, (unsigned) r.mismatch, (unsigned) r.unanswered, (int) r.pending);
    CHECK(r.frames > 100);
    CHECK(r.req == c.req); CHECK(r.match == c.match); CHECK(r.mismatch == 0);
    CHECK(r.unanswered == c.unanswered); CHECK(r.pending == c.pending);
    // alarm block: wherever it was seen it must decode to "None"
    bool seen = r.regs.count(2143);
    if (seen) {
      std::string out; size_t om = 0;
      for (uint16_t reg = 2143; reg <= 2147; reg++) if (r.regs.count(reg)) alarm_bits::describe(reg, r.regs[reg], out, 235, om);
      printf("  alarm block seen: %s\n", out.empty() ? "None" : out.c_str());
      CHECK(out.empty() && om == 0);
    }
  }

  // 3. Alarm decoding
  {
    printf("alarm decoding\n");
    CHECK(alarm_bits::label(2143, 2) == "aux_heater");
    CHECK(alarm_bits::label(2144, 13) == "compressor_error/low_return_line_2");
    CHECK(alarm_bits::label(2143, 0) == "brine:motorprotection_compressor");
    CHECK(alarm_bits::label(2143, 9) == "unnamed");
    std::string out; size_t om = 0;
    alarm_bits::describe(2144, 1u << 13, out, 235, om);
    alarm_bits::describe(2145, 1u << 11, out, 235, om);
    printf("  %s\n", out.c_str());
    CHECK(out == "2144.13 compressor_error/low_return_line_2, 2145.11 internet_gateway_com_error/com_online_card");
    std::string all; om = 0;
    for (uint16_t reg = 2143; reg <= 2147; reg++) alarm_bits::describe(reg, 0xFFFF, all, 235, om);
    printf("  all bits: %u chars, +%u more\n", (unsigned) all.size(), (unsigned) om);
    CHECK(all.size() <= 235 && om > 0);
  }

  printf(fails ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
  return fails ? 1 : 0;
}
