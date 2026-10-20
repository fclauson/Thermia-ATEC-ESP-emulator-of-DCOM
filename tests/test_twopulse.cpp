// test_twopulse.cpp  v2026.10.20  (2026-10-20)  verifies a staged write emits two flag pulses
#include "gateway_responder.h"
#include <cstdio>
#include <vector>
#include <cassert>
using namespace cm930;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)

// Build a mailbox read request frame (FC03, 1800 x6)
static std::vector<uint8_t> mbx_req(){
  std::vector<uint8_t> f={0x0F,0x03,0x07,0x08,0x00,0x06};
  uint16_t c=modbus_sniff::crc16(f.data(),f.size());
  f.push_back(c&0xFF); f.push_back(c>>8); return f;
}

int main(){
  GatewayResponder resp; resp.set_addr(0x0F);
  // Seed register 1053 and its block as "seen" so stage_write is allowed
  // Simulate the HP having written block 1040..1060 (we only need 1053 seen).
  // Observe a FC16 write of reg 1053 = 33.
  std::vector<uint8_t> w={0x0F,0x10,0x04,0x1D,0x00,0x01,0x02,0x00,0x21}; // reg1053=0x041D? 1053=0x041D
  // 1053 = 0x041D. qty1, bc2, value 0x0021=33
  uint16_t c=modbus_sniff::crc16(w.data(),w.size()); w.push_back(c&0xFF); w.push_back(c>>8);
  resp.observe(w.data(), w.size());
  CHECK(resp.has(1053));
  CHECK(resp.get(1053)==33);

  // Stage a write 33 -> 34, flag 1801=4 (idx1=4)
  uint32_t now=100000;
  CHECK(resp.stage_write(1053, 34, 1, 4, now, 15000));
  CHECK(resp.get(1053)==34);

  auto req=mbx_req();
  int pulses=0; bool flag_now=false, prev_flag=false;
  int gap_between=0, since_first=0; int first_pulse_poll=-1, second_pulse_poll=-1;
  // Simulate up to 12 mailbox polls; count how many polls serve 1801=4
  for(int poll=0; poll<12; poll++){
    auto r=resp.build_reply(req.data(), req.size());
    CHECK(r.has_reply());
    // word W1 (1801) is bytes [3],[4] of reply payload: reply = 0F 03 0C W0..W5
    uint16_t w1=(r.bytes[5]<<8)|r.bytes[6];     // W1 = 2nd word -> offset 3+2=5
    flag_now = (w1==4);
    if(flag_now){ pulses++; if(first_pulse_poll<0) first_pulse_poll=poll; else second_pulse_poll=poll; }
    // block value must always be 34 whenever the HP reads it
    resp.after_mailbox_sent();
    resp.tick(now + poll*4200);
  }
  printf("pulses served with 1801=4: %d (first poll=%d second poll=%d)\n", pulses, first_pulse_poll, second_pulse_poll);
  CHECK(pulses==2);                       // TWO pulses like the real gateway
  CHECK(first_pulse_poll==0);             // first pulse immediately
  CHECK(second_pulse_poll>first_pulse_poll+1);  // separated by a quiescent gap
  CHECK(resp.get(1053)==34);              // value held correct throughout

  if(fails==0) printf("ALL TESTS PASSED\n"); else printf("%d CHECK(s) FAILED\n",fails);
  return fails?1:0;
}
