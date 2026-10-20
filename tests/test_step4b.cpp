// HOST-ONLY: keep in tests/, NOT in components/thermia_sniffer/ (own main()).
// Step 4b: staging a 1053 write changes ONLY 1053 in the served 1040-block, clamps to
// 30..55, raises 1801=4 for exactly one poll, and refuses if the block was never seen.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer test_step4b.cpp -o t
#include "modbus_sniff.h"
#include "gateway_responder.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)

static std::vector<uint8_t> fix(std::vector<uint8_t> f){
  uint16_t c=modbus_sniff::crc16(f.data(),f.size()); f.push_back(c&0xFF); f.push_back(c>>8); return f;
}

int main(){
  using R=cm930::GatewayResponder;
  // Build a write of the 1040..1060 block (21 regs) with known values, 1053 = 45.
  // reg index: 1040+i. 1053 is i=13.
  auto make_block=[&](int v1053){
    std::vector<uint8_t> f={0x0F,0x10,0x04,0x10,0x00,0x15,0x2A}; // start 1040, qty 21, bc 42
    for(int i=0;i<21;i++){ int val = (1040+i==1053)? v1053 : (100+i); f.push_back(val>>8); f.push_back(val&0xFF); }
    return fix(f);
  };
  // mailbox read request and 1040 read request
  auto mrq=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
  auto rq1040=fix({0x0F,0x03,0x04,0x10,0x00,0x15});

  // 1. Refused before the block is seen.
  {
    R resp(0x0F);
    CHECK(resp.stage_write(1053, 48, 1, 4)==false);   // unseen -> refused
  }

  // 2. After seeing the block, stage 1053=48, serve the 1040 read: only 1053 changed.
  {
    R resp(0x0F);
    auto blk=make_block(45); resp.observe(blk.data(),blk.size());
    CHECK(resp.get(1053)==45);
    CHECK(resp.stage_write(1053,48,1,4)==true);
    CHECK(resp.get(1053)==48);
    // mailbox now reads 1801=4
    auto mr=resp.build_reply(mrq.data(),mrq.size());
    CHECK(mr.kind==R::Kind::READ);
    // bytes: 0F 03 0C 1800 1801 1802 1803 1804 1805 -> 1801 is bytes[5..6]
    CHECK(mr.bytes[5]==0x00 && mr.bytes[6]==0x04);     // 1801 = 4
    CHECK(mr.was_mailbox);
    resp.after_mailbox_sent();                          // one poll consumed -> flag clears
    auto mr2=resp.build_reply(mrq.data(),mrq.size());
    CHECK(mr2.bytes[5]==0x00 && mr2.bytes[6]==0x00);    // 1801 back to 0
    // now the heat pump reads the 1040 block: 1053 must be 48, everything else unchanged
    auto rb=resp.build_reply(rq1040.data(),rq1040.size());
    CHECK(rb.kind==R::Kind::READ && rb.len==3+21*2+2);
    for(int i=0;i<21;i++){
      int got=(rb.bytes[3+i*2]<<8)|rb.bytes[3+i*2+1];
      int want=(1040+i==1053)?48:(100+i);
      CHECK(got==want);
    }
  }

  // 3. Clamp is the caller's job (component), but stage_write stores whatever it's given;
  //    verify the component-side clamp logic separately here by mimicking it.
  {
    auto clamp=[](int v){ if(v<30)v=30; if(v>55)v=55; return v; };
    CHECK(clamp(10)==30); CHECK(clamp(99)==55); CHECK(clamp(45)==45);
  }

  // 4. Two-pulse contract (matches the real gateway): each pulse shows 1801=4 for exactly one
  //    poll; after the first pulse clears there is a quiescent gap, then a second pulse. The
  //    whole sequence keeps flag_pending() true until the second pulse has been served.
  {
    R resp(0x0F);
    auto blk=make_block(40); resp.observe(blk.data(),blk.size());
    resp.stage_write(1053,50,1,4,100,15000);
    CHECK(resp.flag_pending());
    int pulses=0;
    for (int poll=0; poll<8; poll++) {
      auto mr=resp.build_reply(mrq.data(),mrq.size());
      if (mr.bytes[6]==0x04) pulses++;
      resp.after_mailbox_sent();
    }
    CHECK(pulses==2);                 // exactly two pulses
    CHECK(!resp.flag_pending());       // sequence complete after both
    auto mrq_=resp.build_reply(mrq.data(),mrq.size());
    CHECK(mrq_.bytes[6]==0x00);        // quiescent again
  }


  // 5. Resync pattern is served, and expires by WALL CLOCK even if NO mailbox read is ever
  //    served (the real-world failure: the re-push starves our mailbox replies).
  {
    printf("resync pattern + time-based expiry (no mailbox reads served)\n");
    R resp(0x0F);
    auto mrq2=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
    resp.request_resync(1000, 35000);  // start at t=1000ms, 35s hold
    CHECK(resp.resync_active());
    auto mr=resp.build_reply(mrq2.data(),mrq2.size());
    int v1802=(mr.bytes[7]<<8)|mr.bytes[8], v1803=(mr.bytes[9]<<8)|mr.bytes[10];
    int v1804=(mr.bytes[11]<<8)|mr.bytes[12], v1805=(mr.bytes[13]<<8)|mr.bytes[14];
    CHECK(v1802==32767 && v1803==65535 && v1804==128 && v1805==7);
    // tick with NO after_mailbox_sent() ever called - still active before deadline...
    resp.tick(1000+34000); CHECK(resp.resync_active());
    // ...and reverts after the deadline, without a single mailbox read being counted
    resp.tick(1000+35001); CHECK(!resp.resync_active());
    auto after=resp.build_reply(mrq2.data(),mrq2.size());
    CHECK(((after.bytes[11]<<8)|after.bytes[12])==1919);
    CHECK(((after.bytes[13]<<8)|after.bytes[14])==6);
  }

  // 6. cancel_resync reverts immediately.
  {
    R resp(0x0F);
    resp.request_resync(5000, 35000);
    CHECK(resp.resync_active());
    resp.cancel_resync();
    CHECK(!resp.resync_active());
    CHECK(resp.mailbox(4)==1919 && resp.mailbox(5)==6);
  }

  // 7. The 1801 write flag also has a time backstop: if its mailbox read is starved, tick()
  //    clears it by deadline instead of leaving it raised forever.
  {
    printf("1801 flag time backstop\n");
    R resp(0x0F);
    auto blk=make_block(42); resp.observe(blk.data(),blk.size());
    resp.stage_write(1053, 48, 1, 4, 2000, 15000);  // t=2000ms, 15s backstop
    CHECK(resp.flag_pending());
    // never serve a mailbox read; tick past the backstop
    resp.tick(2000+14000); CHECK(resp.flag_pending());
    resp.tick(2000+15001); CHECK(!resp.flag_pending());
    CHECK(resp.mailbox(1)==0);  // 1801 cleared
    // but the register value stays written
    CHECK(resp.get(1053)==48);
  }

  // 8. Normal case: after the first served mailbox read the 1801 flag VALUE clears to 0
  //    (entering the gap before the second pulse); flag_pending() stays true until the second
  //    pulse is served, then the mailbox is quiescent.
  {
    R resp(0x0F);
    auto blk=make_block(40); resp.observe(blk.data(),blk.size());
    auto mrq3=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
    resp.stage_write(1053, 50, 1, 4, 3000, 15000);
    auto mr=resp.build_reply(mrq3.data(),mrq3.size()); CHECK(mr.bytes[6]==0x04);
    resp.after_mailbox_sent();
    auto mr2=resp.build_reply(mrq3.data(),mrq3.size()); CHECK(mr2.bytes[6]==0x00); // gap: value 0
    CHECK(resp.flag_pending());                                                    // 2nd pulse due
    int pulses=0;
    for (int poll=0; poll<8; poll++) {
      auto m=resp.build_reply(mrq3.data(),mrq3.size());
      if (m.bytes[6]==0x04) pulses++;
      resp.after_mailbox_sent();
    }
    CHECK(pulses==1);                 // the remaining (second) pulse
    CHECK(!resp.flag_pending());
  }


  // 9. Pulse probe: sets 1801 for a couple of SERVED mailbox reads, then clears. Edge-
  //    accurate: a poll that isn't served doesn't count down.
  {
    printf("pulse probe (poll-counted) + clear\n");
    R resp(0x0F);
    auto mrq=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
    resp.probe_once(1, 2, 2);  // 1801=2, hold 2 served polls
    CHECK(resp.probe_active());
    auto mr=resp.build_reply(mrq.data(),mrq.size());
    CHECK(((mr.bytes[5]<<8)|mr.bytes[6])==2);   // 1801=2 served
    CHECK(!resp.probe_after_mailbox_sent());     // 1st served poll, still active
    resp.build_reply(mrq.data(),mrq.size());
    CHECK(resp.probe_after_mailbox_sent());      // 2nd served poll -> pulse ends
    CHECK(!resp.probe_active());
    auto mr2=resp.build_reply(mrq.data(),mrq.size());
    CHECK(((mr2.bytes[5]<<8)|mr2.bytes[6])==0);  // cleared to quiescent
  }
  // 10. Hit detection: a non-mailbox read during the probe is captured; the mailbox poll is not.
  {
    printf("probe hit detection\n");
    R resp(0x0F);
    resp.probe_once(1, 9, 2);
    resp.note_read(1800, 6);          // the mailbox poll itself - must be ignored
    CHECK(resp.probe_hit_qty()==0);
    resp.note_read(1130, 10);         // a real block re-read
    CHECK(resp.probe_hit_start()==1130 && resp.probe_hit_qty()==10);
  }
  // 11. Sweep: steps through a list, one probe per couple of polls, finishes.
  {
    printf("sweep advances and finishes\n");
    R resp(0x0F);
    auto mrq=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
    static const uint16_t IDX[]={1,1}; static const uint16_t VAL[]={2,3};
    resp.sweep_load(IDX, VAL, 2, 1, 1);  // 2 steps, 1-poll hold, 1-poll settle
    CHECK(resp.sweep_active());
    int finished=0, steps=0; bool saw2=false, saw3=false;
    for (int i=0;i<40 && resp.sweep_active();i++){
      resp.build_reply(mrq.data(),mrq.size());
      if (resp.probe_active()){ if(resp.probe_value()==2) saw2=true; if(resp.probe_value()==3) saw3=true; }
      int ev=resp.sweep_after_mailbox_sent();
      if (ev==1) steps++;
      if (ev==2) finished=1;
    }
    CHECK(finished==1);
    CHECK(!resp.sweep_active());
    CHECK(steps==2);       // both steps actually ran (not stuck)
    CHECK(saw2 && saw3);   // both values were actually raised
  }
  // 12. cancel_probe stops a probe and a sweep.
  {
    R resp(0x0F);
    resp.probe_once(0, 1, 2);
    CHECK(resp.probe_active() && resp.mailbox(0)==1);
    resp.cancel_probe();
    CHECK(!resp.probe_active() && resp.mailbox(0)==0);
  }

  printf(fails? "\n%d FAILURE(S)\n":"\nALL TESTS PASSED\n", fails);
  return fails?1:0;
}
