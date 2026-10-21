// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
//
// Alarm bit flags the heat pump writes to the gateway in registers 2143..2147 (its own
// FC16 to 0x085F, separate from the ~21 s live-block rotation). Names come from the
// Thermia Connect 2.1.105 firmware tables (cm930_semantic_objects.csv, category alarm):
//   aq    : air-to-water family, profile 2.3.0-2.4.3 (2143..2145 only)
//   brine : ground-source family, profile 2.2.0-2.2.1 (2143..2147)
// The DHP-AQ is air-to-water, so "aq" is the likely meaning, but the controller's profile
// has NOT been confirmed on this bus. Where the two families disagree both are shown.
// The firmware applies a few extra conditions (e.g. aq 2143.12 only counts if 2144.4 is
// 0; split-only sensors need 1353 > 0). These are NOT applied here: this reports raw bits.
// GENERATED from cm930_semantic_objects.csv -- do not hand-edit the table.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace alarm_bits {

struct AlarmBit {
  uint16_t reg;
  uint8_t bit;
  const char *aq;     // "" = not defined in the air-to-water profile
  const char *brine;  // "" = not defined in the ground-source profile
};

static const AlarmBit TABLE[] = {
    {2143, 0, "", "motorprotection_compressor"},
    {2143, 1, "", "motorprotection_fan"},
    {2143, 2, "aux_heater", "aux_heater"},
    {2143, 3, "", "heat_pressure"},
    {2143, 4, "", "low_pressure"},
    {2143, 5, "", "pressure_transducer"},
    {2143, 6, "", "high_pressure"},
    {2143, 7, "", "flow_switch"},
    {2143, 8, "", "max_temp_card"},
    {2143, 10, "condenser_pump", "condenser_pump"},
    {2143, 11, "system_pump", "system_pump"},
    {2143, 12, "circulation_pump", "brine_pump"},
    {2143, 13, "outdoor_sensor", "outdoor_sensor"},
    {2143, 14, "tank_sensor", "defrost_sensor"},
    {2143, 15, "supply_line_sensor", "supply_line_sensor"},
    {2144, 0, "system_supply_line_sensor", "system_supply_line_sensor"},
    {2144, 1, "return_line_sensor", "return_line_sensor"},
    {2144, 2, "four_way_valve", "high_return_line"},
    {2144, 3, "buffer_tank_sensor", "buffer_tank_sensor"},
    {2144, 4, "", "hgw_sensor"},
    {2144, 5, "tapwater_sensor", "hot_water_sensor"},
    {2144, 6, "tapwater_top_sensor", "hot_water_top_sensor"},
    {2144, 8, "", "brine_in_sensor"},
    {2144, 9, "condenser_in_sensor", "brine_out_sensor"},
    {2144, 10, "condenser_out_sensor", "ref1_sensor"},
    {2144, 11, "ref_1_sensor", "ref2_sensor"},
    {2144, 12, "", "low_return_line_1"},
    {2144, 13, "compressor_error", "low_return_line_2"},
    {2144, 14, "outdoor_unit_error", "low_evap_1"},
    {2144, 15, "outdoor_unit_com_kit_error", "low_evap_2"},
    {2145, 0, "", "suction_temp_sensor"},
    {2145, 1, "room_sensor", "room_sensor"},
    {2145, 2, "old_hub_card_version", "discharge_pipe_sensor"},
    {2145, 3, "distribution_circuit_1_sensor", "dist_circ_1_sensor"},
    {2145, 4, "distribution_circuit_2_sensor", "dist_circ_2_sensor"},
    {2145, 5, "pool_sensor", "pool_sensor"},
    {2145, 6, "", "cooling_sensor"},
    {2145, 7, "outdoor_unit_not_supported", "com_hp_card"},
    {2145, 8, "indoor_hub_card_com_error", "com_hub_card"},
    {2145, 9, "com_kit_com_error", "com_exv_card"},
    {2145, 10, "expansion_card_com_error", "com_exp_card"},
    {2145, 11, "internet_gateway_com_error", "com_online_card"},
    {2146, 0, "", "motorprotectio_compressor_sl"},
    {2146, 1, "", "motorprotectio_fan_sl"},
    {2146, 2, "", "aux_heater_sl"},
    {2146, 3, "", "heat_pressure_sl"},
    {2146, 4, "", "low_pressure_sl"},
    {2146, 5, "", "pressure_transducer_sl"},
    {2146, 6, "", "high_pressure_sl"},
    {2146, 7, "", "flow_switch_sl"},
    {2146, 8, "", "max_temp_card_sl"},
    {2146, 9, "", "condenser_pump_sl"},
    {2146, 10, "", "brine_pump_sl"},
    {2146, 11, "", "defrost_sensor_sl"},
    {2146, 12, "", "supply_line_sensor_sl"},
    {2146, 13, "", "return_line_sensor_sl"},
    {2147, 0, "", "high_return_line_sl"},
    {2147, 1, "", "hgw_sensor_sl"},
    {2147, 2, "", "brine_in_sensor_sl"},
    {2147, 3, "", "brine_out_sensor_sl"},
    {2147, 4, "", "ref1_sensor_sl"},
    {2147, 5, "", "ref2_sensor_sl"},
    {2147, 6, "", "low_return_line_1_sl"},
    {2147, 7, "", "low_return_line_2_sl"},
    {2147, 8, "", "low_evap_1_sl"},
    {2147, 9, "", "low_evap_2_sl"},
    {2147, 10, "", "suction_temp_sensor_sl"},
    {2147, 11, "", "discharge_pipe_sensor_sl"},
    {2147, 12, "", "com_hp_card_sl"},
    {2147, 13, "", "com_hub_card_sl"},
    {2147, 14, "", "com_exv_card_sl"},
};
static constexpr uint16_t FIRST_REG = 2143, LAST_REG = 2147;

// Label for one bit: "aq_name", "aq_name/brine_name", "brine:brine_name" or "unnamed".
inline std::string label(uint16_t reg, uint8_t bit) {
  for (const auto &a : TABLE) {
    if (a.reg != reg || a.bit != bit) continue;
    if (a.aq[0] && a.brine[0]) return strcmp(a.aq, a.brine) == 0 ? std::string(a.aq) : std::string(a.aq) + "/" + a.brine;
    if (a.aq[0]) return a.aq;
    return std::string("brine:") + a.brine;
  }
  return "unnamed";
}

// Every set bit of one register, appended as "2144.13 compressor_error/low_return_line_2".
// Entries are separated by ", ". Once an entry would exceed max_len, it and every later
// one (also across calls with the same counter) are counted in omitted instead.
inline void describe(uint16_t reg, uint16_t value, std::string &out, size_t max_len, size_t &omitted) {
  for (uint8_t b = 0; b < 16; b++) {
    if (!(value & (1u << b))) continue;
    char head[16];
    snprintf(head, sizeof(head), "%u.%u ", (unsigned) reg, (unsigned) b);
    std::string item = std::string(head) + label(reg, b);
    size_t need = item.size() + (out.empty() ? 0 : 2);
    if (omitted > 0 || out.size() + need > max_len) {
      omitted++;
      continue;
    }
    if (!out.empty()) out += ", ";
    out += item;
  }
}

}  // namespace alarm_bits
