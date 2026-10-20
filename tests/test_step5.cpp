// HOST-ONLY: step 5 auto-takeover decision logic. The component's auto_takeover_tick_ uses
// millis()/gateway_replying(), so here we test a faithful mirror of its decision table to
// lock the intended behaviour: never arm if a gateway was heard; arm only after the window.
// Build: g++ -std=c++17 -Wall -Wextra test_step5.cpp -o t
#include <cstdio>
#include <cstdint>

static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)

// Mirror of auto_takeover_tick_ decision. Returns: 0=keep waiting, 1=self-arm, 2=stay disarmed.
struct State { bool auto_on; bool decided; uint32_t boot_ms; uint32_t listen_ms; };
static int decide(State &st, uint32_t now, bool gw_heard) {
  if (!st.auto_on || st.decided) return 0;
  if (now - st.boot_ms < st.listen_ms) {
    if (gw_heard) { st.decided=true; return 2; }  // gateway during window -> stay disarmed
    return 0;                                      // keep waiting
  }
  st.decided=true;
  return gw_heard ? 2 : 1;                          // window elapsed: arm iff no gateway
}

int main(){
  const uint32_t BOOT=1000, LISTEN=20000;
  // 1. Clean cold boot: no gateway for the whole window -> arm at the end.
  {
    State st{true,false,BOOT,LISTEN};
    CHECK(decide(st,BOOT+1000,false)==0);     // waiting
    CHECK(decide(st,BOOT+19999,false)==0);    // still waiting, just before window end
    CHECK(decide(st,BOOT+20000,false)==1);    // window elapsed, no gw -> ARM
    CHECK(st.decided);
    CHECK(decide(st,BOOT+25000,false)==0);    // already decided -> no repeat
  }
  // 2. Gateway heard mid-window -> stay disarmed, never arm.
  {
    State st{true,false,BOOT,LISTEN};
    CHECK(decide(st,BOOT+5000,false)==0);
    CHECK(decide(st,BOOT+8000,true)==2);      // gw heard -> stay disarmed
    CHECK(st.decided);
    CHECK(decide(st,BOOT+25000,false)==0);    // stays decided; no late arm
  }
  // 3. Gateway heard exactly at window end -> stay disarmed.
  {
    State st{true,false,BOOT,LISTEN};
    CHECK(decide(st,BOOT+20000,true)==2);
  }
  // 4. auto_takeover off -> never acts.
  {
    State st{false,false,BOOT,LISTEN};
    CHECK(decide(st,BOOT+50000,false)==0);
    CHECK(!st.decided);
  }
  // 5. Mid-session ESP reboot (gateway already running): gateway heard almost immediately.
  {
    State st{true,false,BOOT,LISTEN};
    CHECK(decide(st,BOOT+200,true)==2);       // heard at once -> stay disarmed
  }
  // 6. The listen window (20s) must end before the earliest observed handshake (27.7s from
  //    HP boot). Since ESP boots within a few s and HP takes 27+s, 20s from ESP boot is safe.
  {
    // earliest handshake from captures = 27.7s after HP first talks; HP boot >= ESP boot.
    const double earliest_hs_s = 27.7;
    const double listen_s = 20.0;
    CHECK(listen_s < earliest_hs_s);  // armed before any handshake can arrive
  }

  printf(fails? "\n%d FAILURE(S)\n":"\nALL TESTS PASSED\n", fails);
  return fails?1:0;
}
