# Thermia DHP-AQ — ESP gateway replacement — HANDOFF

**Last updated:** 20 October 2026
**Status:** Working and tidied (code cleanup done 20 Oct 2026). The ESP fully replaces the
Thermia Online gateway (CM930): it changes settings repeatably (two-pulse writes, all 16 bits
of 1801 mapped, plus 1800 for operation mode), reflects written values back to the display,
self-arms at boot and auto-seeds via resync. The real gateway is retired (20 Oct 2026).
Remaining work is the soak and the cold-weather checks: see §6.

---

## 1. What this is

An ESP32-S3 (ESPHome) sits on the RS485 Modbus bus between a Danfoss/Thermia DHP-AQ heat
pump and its Thermia Online gateway (device address 0x0F, "CM930"). The project reverse-
engineered the gateway protocol and the ESP now **is** the gateway: it answers the heat
pump's security handshake, acknowledges its writes, serves the mailbox, and can push
setting changes back — with no cloud and no real gateway present.

The guiding safety rule throughout: **the ESP only ever serves a register value it has
actually seen the real heat pump use this session.** It never invents, defaults, or clamps
a value into existence. The one block the heat pump reads back (the mailbox) is ESP-authored
from known-constant values.

## 2. Current capabilities (all proven live)

- **Handshake** — answers the heat pump's FC23 challenge (XXTEA, firmware key). Matched
  byte-for-byte live, multiple times.
- **Full gateway emulation** — acks every FC16 write, serves the mailbox. The heat pump
  runs normally with the ESP as its only gateway.
- **Setting writes** — changes settings (e.g. hot water start temp, register 1053) via the
  mailbox re-read mechanism, repeatably (two-pulse flag, matching the real gateway). The heat
  pump adopts the value and shows it on its display, and the ESP's own read-back sensor
  updates once the heat pump has re-read the block.
- **Full-table resync** — can make the heat pump re-push its entire settings table on demand
  (see full-resync-trigger.md).
- **Auto-takeover (on at load)** — the ESP self-arms after a short listen (auto_listen_ms,
  default 8 s) hears no real gateway, then auto-seeds all values via a resync as soon as the
  heat pump addresses it. Set auto_listen_ms back to 20 s only if running with the real
  gateway still on the bus. Proven on a real power cycle.
- **Manual override** — the "Takeover Arm" toggle reflects real state and can disarm the ESP
  until the next reboot without unplugging anything.

**Diagnostics** (added this session, for soak-watching): `Seconds Since HP Update` (resets
whenever the heat pump writes any register — if it climbs without bound the heat pump has
stopped pushing), `Mailbox Reads Served`, `Mailbox Reads Dropped (bus busy)`.

## 3. Key protocol facts (the crown jewels)

- **Gateway address:** 0x0F. Other live devices: 0x02, 0x04 (outdoor unit), 0x06.
- **Bus:** RS485, 9600 baud, 8E1.
- **Handshake:** heat pump sends FC23 `write 1820 / read 1840`, 16-byte challenge. Answer =
  **XXTEA encrypt** of the challenge, 16 bytes (4 words, 19 rounds), words packed
  little-endian. Key (firmware): `BC6725DC 41839788 ED7ACDF7 CA3393A4` (serialized LE =
  bytes `dc2567bc 88978341 f7cd7aed a49333ca`). The heat pump sends 6 attempts ~4.2 s apart
  at boot, and only begins polling the mailbox AFTER a successful handshake.
- **FC16 ack:** 8-byte echo of the request header (addr fc start qty) + CRC.
- **Mailbox:** registers 1800–1805, read by the heat pump ~every 4.2 s.
  - Quiescent: `0, 0, 0, 0, 1919, 6` (1804=1919 and 1805=6 are fixed constants, meaning
    unknown, copied verbatim).
  - `1801 = 1` → heat pump re-reads block 1000–1012 (heating curve, room setpoint).
  - `1801 = 4` → heat pump re-reads block 1040–1060 (hot water, incl. 1053).
  - **Writes need TWO flag pulses.** Proven on our bus: a single online change made the real
    gateway raise the flag at t=11.4 s and AGAIN at t=24.0 s (~2 quiescent polls apart), with
    the heat pump re-reading the block after each pulse. A single pulse applies the value the
    first time but the heat pump treats the second pulse as the commit, so one pulse "works
    once but not subsequently". `stage_write` now emits two pulses automatically.
  - **Read-back display.** After we write a setting, the heat pump READS it from us (FC03) and
    adopts it - it never writes it back, so the display sensor (fed only by heat-pump FC16
    writes) would otherwise stay stale. The component now watches for the heat pump's re-read
    of the block (which only happens because we flagged it) and, when it sees it, publishes the
    committed value to that register's sensor. Only-valid-data: the sensor is updated only once
    the heat pump has actually read the value, never optimistically.
  - `1800 = 1` → heat pump re-reads block 1350–1369 (purpose unclear).
  - Full resync: `1802/1803/1804/1805 = 32767/65535/128/7` → heat pump re-pushes the whole
    1000–1798 table. (full-resync-trigger.md)
- **What the heat pump READS from the gateway:** only the mailbox (1800×6), and block
  1000×13 when flagged. Everything else (settings 1000–1798, live block 2000–2239) the heat
  pump WRITES to the gateway; the ESP only acks.
- **Profile:** air-to-water, firmware 2.1.105, profile 2.3.0–2.4.3 (step3-profile-and-mailbox.md).
- **Alarms:** registers 2143–2147, written by the heat pump at its boot. Bit names in
  alarm_bits.h. 2145.11 = internet-gateway-comms-error (the "lost gateway" flag to watch).

## 4. File inventory

**ESPHome component** (`components/thermia_sniffer/`):
- `__init__.py` — codegen / config schema (slave_address, takeover_capable, flow_control_pin,
  auto_takeover, auto_listen_ms, log_changes, registers list).
- `thermia_sniffer.h` / `.cpp` — the component: decoding, handshake check, gateway shadow,
  responder, takeover (manual + auto, boot-latch-safe), writes (two-pulse) with read-back
  display, resync, and diagnostics (seconds-since-HP-update, mailbox served/dropped).
- `modbus_sniff.h` — RS485/Modbus framer, CRC, decoder.
- `register_store.h` / `reg_ranges.h` — register storage.
- `cm930_handshake.h` — XXTEA handshake (compute + checker). Pure C++, PC-testable.
- `gateway_shadow.h` — predicted-reply-vs-real comparison (step 2). Pure C++.
- `gateway_responder.h` — builds the actual reply bytes, two-pulse write staging, resync,
  and the `is_settings_block_read()` helper (adoption detection). Pure C++.
- `alarm_bits.h` — alarm bit names, generated from the firmware table. Pure C++.

**Config:** `thermia-sniffer.yaml` — the ESPHome device config. Entities grouped into HA
sub-devices. Contains the arming switch, resync button, 1053 write control, all sensors.

**Tests** (`tests/`, HOST-ONLY — never put in the component folder, they have their own
main() and would be compiled into the firmware):
- `test_step1.cpp` — handshake + alarm decode, replayed against captures.
- `test_step2.cpp` — gateway shadow vs real.
- `test_step4a.cpp` — responder builds byte-identical replies; suppresses unseen reads.
- `test_step4b.cpp` — 1053 write + resync behaviour (incl. time-based expiry).
- `test_step5.cpp` — auto-takeover decision logic.
- `test_twopulse.cpp` — a staged write emits TWO flag pulses with a quiescent gap, value held.
- `test_readback.cpp` — settings-block-read detection (block vs mailbox, bounds) used to
  confirm the heat pump's adoption of a written value.
  Build any test: `g++ -std=c++17 -Wall -Wextra -I../components/thermia_sniffer <file> -o t && ./t`
  (the replay tests read the capture .log files from the project; adjust the path in the file.)
  All seven suites pass.

**Docs:**
- `HANDOFF.md` — this file.
- `README-build-and-run.md` — how to build, flash, and operate.
- `modbus-mailbox-flags.md` — CM930 flag-map reference (predicted full 1800–1805 bit map;
  to be confirmed on our bus; reframes the sweep as powers-of-two, bit 6 = 1130 calibration).
- `step3-profile-and-mailbox.md` — profile determination + mailbox semantics.
- `full-resync-trigger.md` — the full-table resync mechanism.
- `NOTE-writable-registers-todo.md` — queued work: more writable registers + flag probing.
- `README-register-logging.md` — GET /registers.json on the ESP, the 5-minute collector on
  Home Assistant (`ha/thermia_collector.py`) and the change report (`ha/thermia_report.py`).
- `NOTE-missing-register-groups.md` — the 8 Thermia Online groups not in the CSV (not needed),
  what is known about each, and how to get their definitions if ever wanted.

## 5. The journey (step by step, all proven)

1. **Step 1** — handshake algorithm (XXTEA) confirmed live (MATCH on the real bus).
2. **Step 2** — full gateway reply model confirmed: zero mismatches over thousands of
   frames, including a real cloud change.
3. **Step 3** — profile, alarm layout, mailbox constants pinned down (desk work).
4. **Step 4a** — takeover: ESP answers as the gateway, changing nothing. Heat pump accepted
   it.
5. **Step 4b** — first write: 1053 changed and adopted by the heat pump, 3×.
6. **Step 5/6** — auto-takeover + auto-seed (resync). Self-arms on a shared power cycle.
   Manual-override toggle.

Along the way we discovered and fixed: a 12 KB stack-temporary crash (don't copy large
objects), a resync feedback loop (hold must be wall-clock, not poll-counted), the
optimistic+lambda number conflict, and learned that the heat pump only polls the gateway
after a successful handshake (so a mid-session ESP-only reboot can't re-establish — a shared
power cycle can).

Later session (Oct 2026) findings and fixes:
- **Writes need TWO flag pulses.** Decoded from a real cloud change: the gateway raised the
  flag at t=11.4 s and again at t=24.0 s, the heat pump re-read the block after each. One
  pulse applies the value once but not again ("works first time only"); the second pulse is
  the commit. `stage_write` now emits two pulses. (`tests/test_twopulse.cpp`.)
- **Read-back display.** After we write, the heat pump READS the value and adopts it but never
  writes it back, so the read-back sensor stayed stale. The component now publishes the
  committed value when it sees the heat pump re-read the block. (`tests/test_readback.cpp`.)
- **Boot-arm latch bug.** The Takeover Arm switch's `restore_mode: ALWAYS_OFF` fired its
  turn-off action at boot, which set the manual-disarm latch, which vetoed auto-takeover every
  boot (symptom: "started unarmed" despite `auto_takeover: true`). Fixed: a disarm in the
  first 4 s after boot no longer latches, so the boot restore can't block auto-takeover; a
  real user toggle still latches.
- **Mailbox-starvation theory disproven** by diagnostics: `Mailbox Reads Served` climbs,
  `Dropped (bus busy) = 0`, `Seconds Since HP Update` stays low. The mailbox is served fine.

## 6. Outstanding actions (20 Oct 2026)

**Status of the 20 Oct list**
- DONE: tidied build flashed and working in ESPHome.
- DONE (live, +1 and back): every write control is now proven on the real heat pump -
  1000-1002, 1006, 1008, 1012 room setpoint, 1053-1056, 1059, 1072-1075, 1077 (negative,
  two's complement) and 1363 operation mode (via 1800).
- DONE: real gateway retired.

**To do**
1. **Soak (underway):** several days fully local. Watch ESP restarts, the `bad=` frame count
   trend (2 during the sweep), and that "Seconds Since HP Update" stays low.
2. **ACTION — cold-weather checks (outstanding)** (needs the first cold spell and defrosts):
   - defrost units: 2172 minutes between the last two defrosts; 2173 compressor run minutes
     since the last defrost (should rise ~1/min only with the compressor, reset at a defrost);
   - ODU values (2091-2096, 2095 bits): fans, valve and defrost bit should follow real behaviour;
   - Pumps & Valves decodes (system enable, test mode still unconfirmed);
   - 2120 integral: compressor should start near -60 (1070) and aux heat near -600 (1111).

**Another day**
3. **Unmapped registers:** use the register logger (README-register-logging.md): let it
   run through heating runs and defrosts, then `thermia_report.py --names ... --only-unmapped`
   shows which unknowns change ("Dump Unmapped Registers" gives a one-off list). Candidates:
   2161/2164/2165/2169/2170 (gaps between the hour counters), 2151 firmware version,
   2180-2239 (unknown live block). Add useful ones to the YAML (unknowns as diagnostic).

**Optional / if wanted**
4. 1070 (integral A1) writable: user to confirm; the NEG_MAGNITUDE encoding is ready.
5. Curve offsets 1003-1005: signed path exists, confirm the encoding with one test write.
6. 1800 (W0) sweep for bits 1-15 (calendar 1370-1768, clock 1770, 1777, 1780); quick wins:
   2151 firmware version and 1770-1776 clock as sensors (NOTE-missing-register-groups.md).
7. Handshake revisit. Note: after an OTA flash (ESP-only reboot) the heat pump kept polling
    the ESP with no new handshake, so the limitation in §7 applies to long gaps, not reboots.
8. Optional rename: "Gateway 0x0F Replying" means a REAL gateway is replying; "Real Gateway
    Detected" would be clearer (changes the entity id).
9. Decide later whether to drop the bring-up diagnostics (gateway shadow, handshake check,
    probe/sweep). Kept for now: they are only active with a real gateway or on demand.

**Done in the 20 Oct cleanup:** step-numbering and receive-only language removed from code
and YAML; dedicated 1053 code path removed (uses the shared write path); duplicate sensor
publishing merged; probe "known blocks" updated to the confirmed map; "sweep 17/16" fixed;
config banner corrected; Takeover Status text no longer carries counters; "Unmapped Registers
Seen" made internal; `state_class: measurement` on live/raw numeric sensors so they stay out
of the HA activity log; obsolete relay placeholder removed from the YAML.

## 7. Known limitations / gotchas

- **Mid-session ESP-only outage.** The heat pump only handshakes at its own boot. A short
  ESP-only reboot (an OTA flash, ~seconds) is fine: on 20 Oct the heat pump kept polling and
  the ESP re-armed with no new handshake. A long ESP outage may make the heat pump declare the
  gateway lost until its next boot (untested). Shared power cuts reboot both, so are fine.
- **~56 ms TX warning** on large (47-byte) block replies: expected wire time at 9600 baud,
  harmless (bad=0 under load), documented in send_frame_. Not worth a risky non-blocking fix.
- **Recovery:** the real gateway is retired. If it is ever reconnected, reboot the heat pump;
  the ESP auto-disarms the moment it hears a real gateway, so two-gateway contention can't
  persist. Without it, recovery is reflashing the ESP (the heat pump runs on its own settings
  meanwhile, just without a gateway).
- **Writes vs a concurrent resync.** A resync makes the heat pump re-push its own table; if
  you write a setting at the same time, the push races the write. Change settings without a
  simultaneous resync.

- **ESPHome entity names must not contain `/`.** It is reserved as a URL path separator:
  ESPHome currently swaps it for a Unicode fraction slash with a warning, and from ESPHome
  2027.7.0 it will be a build error. Use "or", "and" or "-" instead (e.g. the text sensor
  "Last Write or Probe Result").

## 8. Safety model (why this is safe)

- Transmit is impossible unless `takeover_capable: true` AND a flow_control_pin is set.
- The ESP starts DISARMED every boot. It arms only (a) manually via the toggle, or (b) via
  auto-takeover after a short listen (`auto_listen_ms`, default 8 s) confirming no real
  gateway, then auto-seeds values via a resync once the heat pump addresses it. The listen
  window's job is to catch a real gateway if the ESP reboots alone into a running system; at a
  shared cold boot (gateway appears ~30 s in) both short and long windows rely on the
  continuous auto-disarm instead. Set `auto_listen_ms: 20s` if ever running with the real
  gateway still on the bus.
- Arming is refused if the real gateway is replying. If the real gateway is ever heard while
  armed, the ESP auto-disarms within ~50 ms.
- Only one transmit call site in the whole codebase (`send_frame_`), reached only when armed.
- Every served read value was seen on the real bus this session; the mailbox is authored
  from known constants. Every written value is clamped to the register's range, negative
  ranges only via an encoding confirmed on the bus.
