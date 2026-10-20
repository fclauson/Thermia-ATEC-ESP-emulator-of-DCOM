// test_readback.cpp  v2026.10.20  (2026-10-20)  verifies settings-block-read (adoption) detection
#include "gateway_responder.h"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace cm930;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static std::vector<uint8_t> fix(std::vector<uint8_t> f){ uint16_t c=modbus_sniff::crc16(f.data(),f.size()); f.push_back(c&0xFF); f.push_back(c>>8); return f; }

int main(){
  uint16_t start=0,qty=0;
  // Mailbox read 1800x6 -> NOT a settings block
  auto mbx=fix({0x0F,0x03,0x07,0x08,0x00,0x06});
  CHECK(!GatewayResponder::is_settings_block_read(mbx.data(),mbx.size(),0x0F,start,qty));
  // Block 1040 (0x0410) x21 -> IS a settings block, covers 1053
  auto blk=fix({0x0F,0x03,0x04,0x10,0x00,0x15});
  CHECK(GatewayResponder::is_settings_block_read(blk.data(),blk.size(),0x0F,start,qty));
  CHECK(start==1040 && qty==21);
  CHECK(1053>=start && 1053<start+qty);        // 1053 is in the block
  CHECK(!(1061>=start && 1061<start+qty));     // 1061 is NOT (gap after 1060)
  // Wrong address -> no
  auto other=fix({0x04,0x03,0x04,0x10,0x00,0x15});
  CHECK(!GatewayResponder::is_settings_block_read(other.data(),other.size(),0x0F,start,qty));
  // Block 1000 (0x03E8) x13 -> settings block, covers 1000..1012
  auto blk1000=fix({0x0F,0x03,0x03,0xE8,0x00,0x0D});
  CHECK(GatewayResponder::is_settings_block_read(blk1000.data(),blk1000.size(),0x0F,start,qty));
  CHECK(start==1000 && qty==13);
  CHECK(1002>=start && 1002<start+qty);        // heating curve 1002 in block 1000
  if(fails==0) printf("ALL TESTS PASSED\n"); else printf("%d FAILED\n",fails);
  return fails?1:0;
}
