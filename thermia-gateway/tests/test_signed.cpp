// Host-only test (never copy into the component folder): signed/unsigned value decoding.
// Build: g++ -std=c++17 -Wall -Wextra -I../components/thermia_gateway test_signed.cpp -o t && ./t
#include <cstdio>
#include "register_store.h"

static int fails = 0;
static void check(const char *what, float got, float want) {
  bool ok = got == want;
  if (!ok) fails++;
  printf("%-4s %-52s got %8.1f want %8.1f\n", ok ? "ok" : "FAIL", what, got, want);
}

int main() {
  using thermia::decode_value;
  // Two's complement registers (configured signed)
  check("1077 outdoor stop 0xFFEC signed", decode_value(0xFFEC, true), -20);
  check("1004 curve offset 0xFFFF signed", decode_value(0xFFFF, true), -1);
  check("1004 curve offset 0x0001 signed", decode_value(0x0001, true), 1);
  check("1130 calib +5 signed", decode_value(5, true), 5);
  check("1130 calib -5 signed", decode_value((uint16_t) -5, true), -5);
  check("1000 heat curve 22 signed (positive unaffected)", decode_value(22, true), 22);
  // Unsigned registers
  check("2160 compressor hours 21975 unsigned", decode_value(21975, false), 21975);
  check("2160 hours above 32767 stay positive", decode_value(40000, false), 40000);
  check("unsigned 0xFFFF", decode_value(0xFFFF, false), 65535);
  // Magnitude-encoded registers: decode gives the raw magnitude; the YAML filter negates
  check("1070 integral A1 raw 60 (YAML x-1 -> -60)", decode_value(60, true) * -1, -60);
  check("1111 integral A2 raw 60 (YAML x-10 -> -600)", decode_value(60, true) * -10, -600);
  // Encoding for writes: display value -> bus word, and back through the read path
  using thermia::encode_value; using thermia::Encoding;
  check("encode 1077 -20 two's complement = 0xFFEC", encode_value(-20, Encoding::TWOS_COMPLEMENT), 0xFFEC);
  check("encode 1077 -1 two's complement = 0xFFFF", encode_value(-1, Encoding::TWOS_COMPLEMENT), 0xFFFF);
  check("round trip 1077 -15", decode_value(encode_value(-15, Encoding::TWOS_COMPLEMENT), true), -15);
  check("encode 1070 -60 magnitude = 60", encode_value(-60, Encoding::NEG_MAGNITUDE), 60);
  check("encode 1070 -250 magnitude = 250", encode_value(-250, Encoding::NEG_MAGNITUDE), 250);
  check("round trip 1070 -75 (decode, YAML x-1)", decode_value(encode_value(-75, Encoding::NEG_MAGNITUDE), true) * -1, -75);
  printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
