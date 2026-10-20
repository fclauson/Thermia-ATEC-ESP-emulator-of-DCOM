# Step 3 — profile determination and mailbox semantics

**Date:** 18 October 2026
**Purpose:** before anything transmits, pin down (a) which CM930 firmware *profile* this
installation uses, so alarm and register names are certain, and (b) the mailbox constants
and flag values, so a replacement serves them correctly. Desk work only; no bus activity.

All register values below are taken from the 4 Oct 13:49 cold-start capture (the richest
one) and the live step-2 runs. The firmware tables are `cm930_semantic_objects.csv`
(extracted from Thermia Connect 2.1.105 and 2.0.18).

---

## 1. Profile: air-to-water, 2.1.105 / profile 2.3.0-2.4.3

The CM930 firmware ships several profiles. They fall into two families that use the
SAME register numbers for DIFFERENT things:

* **air-to-water** — profiles `1.1.1-2.1.0` and `2.3.0-2.4.3`
* **ground-source / brine** — profiles `2.2.0-2.2.1` and `3.0.0-3.1.1`

### This installation is air-to-water

Decisive physical evidence, not a heuristic:

* **Device 0x04 (the outdoor-unit side) answers** — 189 FC23 replies in the cold-start
  capture. A ground-source unit has no outdoor unit on that bus.
* **Register 2095 exists and decodes** — `odu_operation_status` (= 130 on the bus). This
  register is defined ONLY in the air-to-water profiles. In the brine profiles 2095 is not
  an ODU word at all.
* **2080/2081 are outdoor-unit condenser temperatures** in the air-to-water profile
  (`outdoor_unit_condensor_in/out_temp_measured`); in the brine profile they would be
  `evap_press` / `suction_temp`. The values track the EXV/condenser behaviour the YAML
  already decodes.
* **2036 = `outdoor_unit_status`** in air-to-water; `oper_data_port_d_hp_card` in brine.

A naive min/max range-fit scored the brine profiles slightly higher (10/11 vs 7/11),
because three air-to-water registers (1060, 1070, 1114) hold values just outside the CSV's
stated limits. Those limits are the cloud-UI *editing* limits, not hard bus constraints, so
an out-of-range value does not rule the profile out. The physical evidence above is
authoritative and points the other way.

### Which air-to-water profile

`1.1.1-2.1.0` and `2.3.0-2.4.3` have **identical register names** wherever they overlap, so
the register data alone cannot separate them. Use **2.3.0-2.4.3**:

* it covers 515 of the registers seen on the bus, vs 509 for 1.1.1-2.1.0 (it is the superset
  and the newer profile);
* it is the profile used for the alarm bit table already generated in `alarm_bits.h`.

The only practical consequence of the choice is a few alarm-bit names (below). The
non-alarm register names are the same either way.

### Confidence

HIGH for "air-to-water". The alarm names should be read as "2.3.0-2.4.3, with 1.1.1-2.1.0
as the only alternative", i.e. a handful of bits could carry the other profile's name. The
firmware does not expose a single "profile = X" register we could read to remove that last
ambiguity; it is selected internally. `2151` (HP firmware version) reads **22** here, which
is < 200, consistent with no separately-numbered outdoor-unit generation.

---

## 2. Alarm bits 2143-2147 (confirmed layout)

The heat pump writes 2143-2147 to the gateway as its own FC16 to **0x085F** (= 2143), five
registers. This is separate from the ~21 s live-block rotation; it is sent at boot and,
by inference, when an alarm changes (never yet seen changing — all zero in every capture).

Air-to-water (2.3.0-2.4.3) uses **2143, 2144, 2145** (+ 2095.10). 2146/2147 are
"slave unit" words used only by the brine profiles and are expected to stay 0 here.

The full bit list is in `alarm_bits.h` (generated from the firmware table) and is surfaced
by the `Active Alarm Bits` entity. The names to trust for this installation are the
air-to-water ("aq") names. Notable bits: 2145.11 = `internet_gateway_com_error` — the one
to watch during the step-4 live takeover, as it is how the heat pump would flag that it has
lost its gateway.

---

## 3. Mailbox 1800-1805 (transport layer, not in the semantic tables)

The mailbox is the HP↔CM transport signalling, below the semantic register layer, so it
does **not** appear in `cm930_semantic_objects.csv`. Its behaviour is established entirely
from captures and the live step-2 runs.

### Quiescent contents (every capture)

```
1800 = 0
1801 = 0
1802 = 0
1803 = 0
1804 = 1919   (0x077F)   constant
1805 = 6                 constant
```

A replacement must serve exactly `0, 0, 0, 0, 1919, 6` when no re-read is pending. The
gateway shadow (step 2) seeds and checks exactly these values; 1407+ real mailbox reads
matched. 1804=1919 and 1805=6 are fixed; their precise meaning is unknown (likely a CM
capability/version word), and copying them verbatim is correct and sufficient. There is no
need to decode them further for a replacement.

### Flag → block-reread map (both confirmed on THIS bus)

| Mailbox write | Heat pump then reads | Confirmed by |
|---|---|---|
| `1801 = 1` | block **1000-1012** (`1000 x13`) | two online curve/room-setpoint changes |
| `1801 = 4` | block **1040-1060** (`1040 x21`) | live step-2 run, 17 Oct: Legionella interval changed online |
| `1800 = 1` | block **1350-1369** | seen twice, content unchanged, purpose still unclear |

The step-2 run on 17 Oct captured a complete `1801=4` cycle end to end:
`1801:0->4` raised → HP reads `1040 x21` → `1801:4->0` cleared, all within ~4 s, with the
shadow classifying it correctly (mailbox event + read-change, zero model bugs).

No other 1801 or 1800 values have ever been observed. 1802/1803 have only been seen at 0
except the brief extreme-value burst that precedes a gateway-power-cycle resync (noted in
the handoff, not a normal flag).

---

## 4. 2148-2151 (adjacent block, for completeness)

Written as `0864 x4` (= 2148). Decodes as:

```
2148 = operation/queued low word (seen 0, briefly 2)
2149 = operation_status  (bits: heat, hot water, top-up, pool, defrost x2, outdoor-stop, cooling, concrete-drying)
2150 = queued_demands    (bits: heating, hot water, pool, cooling, concrete-drying)
2151 = hp_cm_firmware_version = 22
```

The YAML's existing 2149/2150 bit decoders match this profile's firmware names exactly — no
change needed.

---

## 5. What remains open after step 3

Nothing that blocks a replacement, and nothing that requires the real gateway to resolve:

* **Last profile ambiguity** (2.3.0-2.4.3 vs 1.1.1-2.1.0): affects only a few alarm-bit
  names, and only matters if one of those specific alarms ever fires. Not safety-relevant.
* **1804=1919 / 1805=6 meaning**: unknown, but copied verbatim, which is sufficient.
* **1800=1 → 1350-1369 block purpose**: still unclear, content never changes. A replacement
  can mirror the real gateway (raise it when it would, serve the block from store).

Ready for step 4 (first live takeover) whenever you choose, with the known recovery
(plug the gateway back in, reboot the heat pump) always available.
