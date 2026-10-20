// HOST-ONLY: keep in tests/, NOT in components/thermia_sniffer/ (it has its own main()).
// Step 4a: the responder must build replies BYTE-IDENTICAL to the real gateway, and must
// suppress (not invent) any read whose registers it has not seen this session.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer test_step4a.cpp -o t
#include "modbus_sniff.h"
#include "gateway_responder.h"
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
static std::string hex(const uint8_t *p, size_t n) {
  std::string s; char b[3];
  for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", p[i]); s += b; }
  return s;
}

// Replay a capture: feed every frame to observe(); when a frame is a request addressed to
// the gateway, build the reply and compare it to the gateway's ACTUAL next frame.
struct Res { uint32_t compared, identical, differ, suppressed_seen_ok, frames; };

static Res replay(const std::string &path, bool verbose) {
  Res r{0,0,0,0,0};
  cm930::GatewayResponder resp(0x0F);
  modbus_sniff::Framer fr(3000, 30000);
  std::ifstream f(path);
  std::string line;
  std::vector<std::vector<uint8_t>> frames;
  // First collect all CRC-valid frames in order.
  uint32_t last_end = 0;
  auto drain = [&](uint32_t now) {
    const uint8_t *d; size_t n;
    while (fr.poll(now, &d, &n)) frames.emplace_back(d, d + n);
  };
  while (std::getline(f, line)) {
    std::istringstream is(line); double t; std::string h;
    if (!(is >> t >> h)) continue;
    auto b = unhex(h);
    uint32_t us = (uint32_t)(t*1e6); if (us < last_end+4000) us = last_end+4000;
    drain(us);
    for (size_t i=0;i<b.size();i++) fr.push(b[i], us+(uint32_t)(i*1146));
    last_end = us + (uint32_t)(b.size()*1146);
    drain(last_end+3500);
  }
  r.frames = frames.size();
  // Walk frames; a request to 0x0F is followed (usually next frame) by the gateway reply.
  for (size_t i=0;i<frames.size();i++) {
    auto &fr0 = frames[i];
    resp.observe(fr0.data(), fr0.size());
    if (fr0.size()<4 || fr0[0]!=0x0F) continue;
    uint8_t fc = fr0[1];
    bool is_req = (fc==0x17 && fr0.size()>13) || (fc==0x10 && fr0.size()>8) || (fc==0x03 && fr0.size()==8);
    if (!is_req) continue;
    auto reply = resp.build_reply(fr0.data(), fr0.size());
    // find the gateway's actual reply = the next 0x0F frame that is a reply shape
    const std::vector<uint8_t>* act=nullptr;
    for (size_t j=i+1;j<frames.size() && j<i+4;j++){
      auto &g=frames[j];
      if (g.size()<4||g[0]!=0x0F) continue;
      if (g[1]==fc){ act=&g; break; }
    }
    if (reply.has_reply()) {
      if (act) {
        r.compared++;
        bool same = (act->size()==reply.len) && memcmp(act->data(),reply.bytes,reply.len)==0;
        if (same) { r.identical++; }
        else {
          // Allowed divergence: a mailbox read (1800x6) where the REAL gateway had raised a
          // re-read flag (1801/1800 nonzero) due to a cloud change. In 4a we never raise a
          // flag ourselves, so serving quiescent is correct; this is not a wrong reply.
          bool mailbox = (fc==0x03 && fr0.size()==8 && fr0[2]==0x07 && fr0[3]==0x08 && fr0[5]==0x06);
          bool real_flag = act->size()>=9 && (act->data()[3]||act->data()[4]||act->data()[5]||act->data()[6]);
          if (mailbox && real_flag) { r.suppressed_seen_ok++; if (verbose) printf("    (mailbox flag event, expected divergence: real had a raised flag)\n"); }
          else { r.differ++; if (verbose) printf("    DIFFER fc=%02x req=%s\n      ours=%s\n      real=%s\n",
               fc, hex(fr0.data(),fr0.size()).c_str(), hex(reply.bytes,reply.len).c_str(), hex(act->data(),act->size()).c_str()); }
        }
      }
    } else {
      // we suppressed. If the gateway DID reply, that's a read we had to stay silent on.
      if (act && fc==0x03) r.suppressed_seen_ok++;  // expected for unseen blocks mid-session
    }
  }
  return r;
}

int main() {
  // Unit: handshake reply byte-identical to the genuine pair.
  {
    printf("unit: handshake / ack / read / suppression\n");
    cm930::GatewayResponder resp(0x0F);
    auto rq = unhex("0f1707300008071c0008102ee156572b8b84a0bc7a65c72d56185d7840");
    auto reply = resp.build_reply(rq.data(), rq.size());
    CHECK(reply.kind==cm930::GatewayResponder::Kind::HANDSHAKE);
    CHECK(hex(reply.bytes,reply.len)=="0f171020b8f28a236f28fa0339669e2c6f600d57d9");
    // ack: write to 0834 x18, data -> 8-byte ack identical to the real one seen on bus
    auto wr = unhex("0f1008340012"); // header only; append bc+data+crc
    std::vector<uint8_t> full = {0x0F,0x10,0x08,0x34,0x00,0x12,0x24};
    for(int i=0;i<0x24;i++) full.push_back(0);
    uint16_t c=modbus_sniff::crc16(full.data(),full.size()); full.push_back(c&0xFF); full.push_back(c>>8);
    auto ack=resp.build_reply(full.data(),full.size());
    CHECK(ack.kind==cm930::GatewayResponder::Kind::ACK);
    CHECK(hex(ack.bytes,ack.len)=="0f10083400120284");
    // mailbox read -> quiescent, always available
    auto mrq=unhex("0f03070800064450");
    auto mr=resp.build_reply(mrq.data(),mrq.size());
    CHECK(mr.kind==cm930::GatewayResponder::Kind::READ);
    CHECK(hex(mr.bytes,mr.len)=="0f030c0000000000000000077f0006adda");
    // unseen read -> SUPPRESSED, no bytes
    auto urq=unhex("0f0303e8000d0551"); // read 1000 x13, never written yet
    auto ur=resp.build_reply(urq.data(),urq.size());
    CHECK(ur.kind==cm930::GatewayResponder::Kind::SUPPRESSED_UNSEEN && !ur.has_reply());
    // after observing a write to 1000 x13, the read is served and matches those values
    std::vector<uint8_t> w1000={0x0F,0x10,0x03,0xE8,0x00,0x0D,0x1A};
    for(int i=0;i<13;i++){ w1000.push_back(0); w1000.push_back((uint8_t)(20+i)); }
    uint16_t c2=modbus_sniff::crc16(w1000.data(),w1000.size()); w1000.push_back(c2&0xFF); w1000.push_back(c2>>8);
    resp.observe(w1000.data(),w1000.size());
    auto sr=resp.build_reply(urq.data(),urq.size());
    CHECK(sr.kind==cm930::GatewayResponder::Kind::READ && sr.has_reply());
    CHECK(sr.bytes[3]==0 && sr.bytes[4]==20 && sr.bytes[5]==0 && sr.bytes[6]==21);
  }

  // Replay every capture: our replies must be byte-identical to the gateway's.
  const std::string dir="../captures/";
  const char* files[]={
    "thermia_capture_20261004_134926_-_cold_start_up.log",
    "thermia_capture_20260924_210001_on_line_change_-_reflected_down_to_HP.log",
    "thermia_capture_20260924_210001_at_HP_change_of_value_and_it_appears_on_line_.log",
  };
  for (auto fn: files) {
    printf("%s\n", fn);
    Res r = replay(dir+fn, true);
    printf("  frames=%u compared=%u identical=%u DIFFER=%u suppressed(unseen read)=%u\n",
           r.frames, r.compared, r.identical, r.differ, r.suppressed_seen_ok);
    CHECK(r.differ==0);        // never build a wrong reply
    CHECK(r.identical>0);      // did actually match some
  }

  printf(fails ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
  return fails?1:0;
}
