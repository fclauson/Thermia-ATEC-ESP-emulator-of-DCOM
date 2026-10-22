# Note — registers to expose as settable HA controls

> **UPDATE 20 Oct 2026 — current state (supersedes the sections below where they differ).**
> All 16 bits of 1801 are confirmed by the sweep (modbus-mailbox-flags.md), so every lower
> settings block has a delivery flag. User decisions on what is writable from HA:
>
> | Block (1801 flag) | Writable from HA | Read-only (change on the HP) |
> |---|---|---|
> | 1000 (1) | 1000, 1001, 1002, 1006, 1008, 1012 room setpoint (not in CSV; 10–30 °C assumed, refused if current value is outside that) | 1003–1005 pending (signed path exists; encoding to confirm) |
> | 1040 (4) | 1053, 1054, 1055, 1056, 1059 (select) | 1061 never on the bus |
> | 1070 (8) | 1072, 1073, 1074, 1075, 1077 (signed, two's complement) | 1070 (by decision, may change) |
> | 1090 (16) | — | 1090 cooling (by decision) |
> | 1110 (32) | — | 1110 (CSV), 1111–1114 (by decision) |
> | 1130 (64) | — | 1130–1139 calibration (by decision) |
> | 1350 (1800=1) | 1363 operation mode (select, via `set_writable_reg_upper()`) | 1369 link integration (path exists if wanted) |
>
> **All writable controls above are live-tested on the real heat pump (20 Oct 2026),**
> +1 and back, including 1077 (negative) and 1363 (via 1800).
>
> Negative-range writes go through `set_writable_reg_signed()` with an encoding confirmed by
> reading the register (`thermia::Encoding`); the generic `set_writable_reg()` still refuses
> any negative range.

> **UPDATE 19 Oct 2026:** Groups 1 and 2 below are now DONE — write controls added to the
> YAML (Heating Curve, Curve Min/Max, Heat Stop, Room Factor; Hot Water Time, Hot Water Top
> Heating Time), using a generic `set_writable_reg()` that reuses the proven 1053 path.
> Curve fine-tune points 1003/1004/1005 (-5..+5) were left out pending signed-value handling.
> Group 3 (calibration) still needs its mailbox flag mapped — since done: see modbus-mailbox-flags.md (UPDATE 20 Oct 2026).

**Raised:** 18 October 2026, by the user, after step 4a was built.
**Status:** NOT YET IMPLEMENTED. This is a to-do for a step after 4a/4b, once writing a
value live is proven (currently only 1053 is the planned first write).

The user wants these registers made settable from Home Assistant. All limits below are from
`thermia-register-definitions.csv` (the Thermia Online register definitions). Every one is
`read_only: False` with `step: 1`. They should all appear in the full settings table the
heat pump pushes at its own boot (block 1000–1798), so the ESP will have a current baseline
for them once it has seen one boot-table push this session (the existing rule).

## 1. Heating curve group (block 1000–1012, delivered by mailbox flag 1801=1 — CONFIRMED)

| Reg | Name | Min | Max | Step |
|---|---|---|---|---|
| 1000 | REG_HEATING_HEAT_CURVE | 22 | 56 | 1 |
| 1001 | REG_HEATING_HEAT_CURVE_MIN | 10 | 50 | 1 |
| 1002 | REG_HEATING_HEAT_CURVE_MAX | 40 | 85 | 1 |
| 1003 | REG_HEATING_CURVE_PLUS5 | -5 | 5 | 1 |
| 1004 | REG_HEATING_CURVE_0 | -5 | 5 | 1 |
| 1005 | REG_HEATING_CURVE_MINUS5 | -5 | 5 | 1 |
| 1006 | REG_HEATING_HEAT_STOP | 0 | 40 | 1 |
| 1008 | REG_HEATING_ROOM_FACTOR | 0 | 4 | 1 |

These sit in the 1000–1012 block, which the heat pump re-reads on mailbox flag **1801=1**
(confirmed twice from real online changes). So the delivery path for these is already known
and matches the 1053 path, just a different flag.

**NOT settable — leave as read-only telemetry** (grouped under HEATING_CURVE in the CSV but
`read_only: True`, no limits; they are live "desired temperature" outputs, not settings):
1. 2123 REG_DESIRED_DISTR_CIR1
2. 2124 REG_DESIRED_DISTR_CIR2

## 2. The two named hot-water times (block 1040–1060, mailbox flag 1801=4 — CONFIRMED)

| Reg | Name | Min | Max | Step | Unit |
|---|---|---|---|---|---|
| 1054 | REG_SER_HOT_WATER_TIME | 5 | 40 | 1 | (minutes) |
| 1056 | REG_HOT_WATER_TOP_HEATING_TIME | 0 | 10 | 1 | (hours) |

Both are in the 1040–1060 block — the SAME block as 1053 (hot water start temp), delivered
by mailbox flag **1801=4** (confirmed by the Legionella change on 17 Oct). So once the 1053
write is proven, these two come essentially for free: same block, same flag, limits above.

## 3. Sensor Calibration group — limits confirmed, DELIVERY FLAG NOT YET KNOWN

All ten are offsets, range **-5 to +5**, step 1:

| Reg | Name |
|---|---|
| 1130 | REG_SENSOR_CALIB_OUTDOOR |
| 1131 | REG_SENSOR_CALIB_SYS_SUPPLY_LINE |
| 1132 | REG_SENSOR_CALIB_SUPPLY_LINE |
| 1133 | REG_SENSOR_CALIB_RETURN_LINE |
| 1134 | REG_SENSOR_CALIB_HOT_WATER |
| 1135 | REG_SENSOR_CALIB_HOT_WATER_TOP |
| 1136 | REG_SENSOR_CALIB_REFR_1_BRINE_IN |
| 1137 | REG_SENSOR_CALIB_REFR_2_BRINE_OUT |
| 1138 | REG_SENSOR_CALIB_DISCH_PIPE |
| 1139 | REG_SENSOR_CALIB_DEFROST_SENSOR |

**Open item:** these live in the **1130-block**, which is NOT one of the two blocks we have
a confirmed mailbox re-read flag for (we only know 1801=1 → 1000–1012, 1801=4 → 1040–1060,
and 1800=1 → 1350–1369). So before these can be made to take effect live, we must establish
which mailbox flag (if any) makes the heat pump re-read the 1130 block. Likely candidates to
test once we are writing: watch what flag the real gateway raises when a calibration value is
changed online (same method that mapped 1801=1 and 1801=4). Until that is known, calibration
values could be written to the ESP's store but we cannot yet guarantee the heat pump adopts
them.

## Implementation sketch (for whoever picks this up)

- Expose each as an ESPHome `number` entity (min/max/step as above), grouped in its existing
  sub-device (Heating & Curve, Hot Water, Sensor Calibration).
- On change: clamp to the limits above, write the value into the responder's store for that
  register, then raise the correct mailbox flag so the heat pump re-reads the block.
- Keep the only-valid-data rule: only serve the block once the ESP has seen it pushed this
  session; clamp every written value to its min/max before it is ever served.
- Groups 1 and 2 are ready to implement as soon as the 1053 write is proven. Group 3 needs
  the 1130-block re-read flag identified first.

## Prerequisite

None of this is safe to attempt before step 4b (first live write, 1053) is proven. This note
is a queue of work, not something to act on now.

---

## Update (18 Oct 2026) — how to map the Sensor Calibration (1130-block) flag

The online-change method CANNOT be used for the calibration block: Sensor Calibration is
not exposed in the user's version of Thermia Online (locked for security reasons). So there
is no way to make a cloud change that would raise the flag for us to observe passively.

**Plan instead — flag probing once the ESP is the full emulator (after step 4b):**

When the ESP has fully replaced the gateway and the real gateway is gone, WE own the
mailbox. At that point, to find which flag re-reads a given block:

1. With the ESP armed and acting as gateway, set a mailbox flag to a candidate value
   (e.g. try 1800 / 1801 across the values we have not yet mapped) for exactly one poll.
2. Watch what block the heat pump ("the master") then reads back.
3. Record flag value -> block. This is the same observation we made passively for
   1801=1 and 1801=4, but now driven by us instead of the cloud.

This turns the unknown calibration-block flag (and any other block's flag) into something
we can discover by direct experiment, safely, because by then takeover and recovery are
already proven. Do it one flag value at a time, logged, bus-idle-checked, with the heat
pump watched — same discipline as everywhere else.

**Only applies after step 4b.** Noted here so it is not forgotten: once the emulator is
complete, probe each mailbox flag value and record which block the master asks for. That
closes the delivery path for calibration (1130-block) and confirms/extends the flag map for
every other block too.

---

## UPDATE 19 Oct 2026 — writes need TWO flag pulses

The "works once but not subsequently" write bug is solved. Ground truth from the real
gateway (capture `..._on_line_change_-_reflected_down_to_HP.log`): ONE online change produced
TWO flag pulses on 1801, at t=11.4 s and t=24.0 s (~2 quiescent polls apart), and the heat
pump re-read the settings block after EACH pulse. A single pulse applies the value but the
heat pump needs the second pulse to commit, so one-pulse writes take the first time only.

`stage_write` now emits two pulses automatically (pulse → ~2-poll gap → pulse), so this
applies to every writable register, not just 1053. Verified by `tests/test_twopulse.cpp`.
