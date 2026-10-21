# Note — register groups not covered by the CSV

**Written:** 20 October 2026
**Status:** Not needed for the build. Recorded so the gap is known, and so it can be
closed later if one of these groups becomes useful.

## Background

Thermia Online organises registers into 20 groups (`REG_GROUP_*`, plus `SPF`).
`thermia-register-definitions.csv` only defines registers for 12 of them. Those 12 are all
implemented in `thermia-gateway.yaml`, each in its own HA sub-device.

The exceptions are:

- HEAT_PUMP is folded into "Heating & Curve".
- OPERATIONAL_STATUS shares "Operation Mode & Status" with OPERATIONAL_OPERATION.

The other 8 groups have no names, limits or enums in this project. **Decision: they are
not required.** This note records what little is already known about each one and how to
get the full definitions if they are ever wanted.

## The 8 groups not covered

| Group | What we already know | Where it would come from |
|---|---|---|
| REG_GROUP_DEVICETIME | The HP's own clock is at **1770–1776** (mailbox W0 bit 13). It is pushed at every boot and resync, so it is already in the store, but no sensors exist for it. | Bus evidence is enough. Decode 1770–1776 from a resync capture against the HP display clock. |
| REG_GROUP_SOFTWARE_VERSIONS | **2151** = HP/CM firmware version, reads 22 (step3-profile-and-mailbox.md §4). It is not in the YAML. | `cm930_semantic_objects.csv` for any other version registers. 2151 could be added directly. |
| REG_GROUP_POOL | Only status bits are known: 2149 "pool heating", 2150 "queued demand: pool", and the alarm bit 2145.5 `pool_sensor`. No pool setting registers are known. This install likely has no pool. | Semantic table. Pool registers may be absent or unused on this unit. |
| REG_GROUP_BUFFER_TANK | **2125** (desired buffer tank temp) is implemented, but the CSV files it under TEMPERATURES. There is also the alarm bit 2144.3 `buffer_tank_sensor`. | Semantic table. |
| REG_GROUP_TEMPERATURES_SLAVE | Only the slave alarm words **2146/2147** are known. They are brine-profile only and expected to stay 0 on this air-to-water unit. | Semantic table. This group is probably not relevant to air-to-water. |
| REG_GROUP_IMMERSION_HEATER_AUX | Nothing specific is known. | Semantic table. |
| REG_GROUP_CALENDAR | The blocks are known: **1370–1768**, mailbox W0 bits 1–12, 33 registers each. **1436** is one of the two blocks the HP retries forever without a gateway. None of the contents are decoded. | Semantic table for the layout. Bus evidence for the contents (see below). |
| SPF | Seasonal performance factor. Probably calculated in the cloud, like the 7000-range registers 7002/7003, rather than sent on the bus. Unconfirmed. | Thermia Online / CM930 firmware. If it is cloud-only, it would have to be computed in HA from energy data. |

## How to get the definitions if needed

1. **Semantic table (best source).** Recover `cm930_semantic_objects.csv`, which was
   extracted from Thermia Connect 2.1.105 and 2.0.18 and used in step 3. Filter it to the
   air-to-water profile 2.3.0–2.4.3 and join it by group. That gives register numbers,
   names and limits for each missing group.

   It is CM930 (Thermia Connect), not our Thermia Online gateway. Block grouping matches
   our bus, but treat every entry as "to confirm on our bus" (see modbus-mailbox-flags.md
   §4).

2. **Thermia Online export.** The existing CSV was exported from Thermia Online register
   definitions. If the export can be repeated with all groups included, it would give the
   cloud's own limits and enums in the same format. This route is now harder, since the
   real gateway is being retired.

3. **Bus evidence (no external source needed).** Everything in 1000–1798 is already pushed
   at boot and on Request Full Resync, and 2000–2239 arrives in the live rotation. Two
   existing tools show what we hold but haven't named:
   - the "Unmapped Registers Seen" text sensor;
   - the "Dump Unmapped Registers" button.

   To decode a register, change the setting on the HP display. Then use "Log Changed
   Registers Since Last Logged" to see which register moved.

4. **Calendar specifically.** The calendar blocks can be confirmed with the mailbox probe
   on **1800** (W0) using powers of two: bit 1 = value 2 → 1370, bit 2 = value 4 → 1403,
   and so on. Follow the same seed → probe → observe discipline as
   mailbox-flag-probing-plan.md. Contents are then decoded by changing a schedule on the HP
   display and diffing.

## Quick wins (if ever wanted)

Two items could be added without any new research, because the registers are already
known and in the store:

- **2151** — firmware version, as a sensor under a "Software Versions" sub-device.
- **1770–1776** — HP clock, as a "Device Time" text sensor.

Neither needs any bus change.
