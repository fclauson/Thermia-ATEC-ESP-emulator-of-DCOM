# Mailbox flag probing — experiment plan

**Purpose:** discover, by direct experiment, which mailbox flag value makes the heat pump
re-read which block. This closes the delivery path for registers whose flag we don't yet
know (the Sensor Calibration 1130-block being the main one), and confirms/extends the flag
map for every other block.

**Why probing (not the cloud):** the usual way to learn a flag is to change a setting in
Thermia Online and watch which flag the real gateway raises. But calibration is locked in
this user's Thermia Online, so that route is closed. Since the ESP now owns the mailbox, we
can raise candidate flags ourselves and watch what the heat pump reads back.

**Prerequisite:** this is only safe once takeover is proven (done) and the ESP is armed as
the sole gateway. Do it deliberately, watched, with the real gateway on the shelf for
recovery.

---

## The key sequencing insight (from the user)

**Do a full refresh FIRST, so we hold valid numbers, THEN probe flags one at a time.**

The reason this ordering matters: when we raise a flag and the heat pump re-reads a block,
the ESP has to *serve that block*. Under the only-valid-data rule it can only serve a block
it has seen this session. So before probing, we force a full table push (the resync
trigger) to fill the store with the heat pump's real current values. After that, whatever
block the heat pump asks to re-read, we can serve it truthfully (unchanged), and simply
observe WHICH block it asked for. We change nothing — we're only learning the flag→block
map, not writing values.

So the method is: **seed (resync) → raise one candidate flag → observe which block the heat
pump reads → revert the flag → record → repeat.** Each probe is read-only in effect (we
serve the real values we already hold), so it's low risk.

---

## What we already know (the map so far)

| Mailbox write | Heat pump re-reads | How confirmed |
|---|---|---|
| 1801 = 1 | block 1000–1012 | online curve change (cloud), and the 1053 path |
| 1801 = 4 | block 1040–1060 | online Legionella change; our own 1053 write |
| 1800 = 1 | block 1350–1369 | observed twice, purpose unclear |
| 1802/1803/1804/1805 = 32767/65535/128/7 | whole 1000–1798 table | gateway power-cycle; our resync button |

Unknown: the flag for the 1130-block (calibration), and whether 1801 has values 2, 3, 5, 6…
that map to other blocks, and what 1800=1 (block 1350) is actually for.

---

## Procedure (per probe)

1. **Seed.** Armed, press "Request Full Resync". Wait until the table push completes and the
   mailbox reverts to quiescent. The store now holds the heat pump's real current values for
   all of 1000–1798.
2. **Baseline.** Note the log is quiet (normal mailbox polls, no block re-reads).
3. **Raise one candidate flag** for a short hold (a few polls). Candidates, in order:
   - 1801 = 2, then 3, then 5, then 6, then 7 (we know 1 and 4; fill in the gaps)
   - 1800 = 2, 3, 4 (we know 1)
   - any other single mailbox register nudged to a small value
4. **Observe.** Watch the log for an FC03 read request from the heat pump to 0x0F for a block
   OTHER than the mailbox. Record: flag value → block start/length. The ESP serves that block
   from its (real, seeded) store, so nothing changes.
5. **Revert** the flag to quiescent (or let the one-poll/again time-based hold expire).
6. **Record** the result and repeat with the next candidate.

**Specifically for calibration:** once a flag is found that makes the heat pump re-read the
1130-block, that's the delivery path for the ten calibration registers
(NOTE-writable-registers-todo.md §3). If NO flag triggers a 1130 re-read, that tells us the
calibration block isn't deliverable by the mailbox mechanism at all — also a useful result.

---

## Safety and stop rules

- **Change no values during probing.** Serve every block from the seeded store unchanged.
  We're mapping flags, not writing. (A value write is a separate, later step, using the
  flag we just mapped.)
- **One flag at a time, short holds.** Never raise several at once; it muddies which flag
  caused which read, and extreme multi-register values are the resync trigger.
- **Avoid the resync pattern by accident.** 1802/1803 at extreme values = full resync. Keep
  probe values small and single.
- **Watch for alarms.** If anything unexpected happens (an alarm bit sets, the heat pump
  behaves oddly), revert to quiescent, disarm, and if needed plug the gateway back in and
  reboot the heat pump.
- **Expect most probes to do nothing.** A flag value with no mapping simply produces no
  block re-read. That's a valid "no mapping" result, not a failure.

---

## Tooling note (not yet built)

Probing by hand needs a way to set an arbitrary mailbox register to an arbitrary value for a
short hold. The component already has the internals (`responder_.set_mailbox(idx, value)`
and the time-based hold used by the resync). A small addition would expose a "probe" control:
set mailbox index N to value V for ~3 polls, then revert. That's a clean, low-risk next
build when you're ready to run this experiment. Until then, the resync button + reading the
log covers the full-table case.

---

## Predicted full bit map (from CM930 modbus-flags.json) — 19 Oct 2026

A CM930 firmware flag map is now available (see `modbus-mailbox-flags.md` in the bundle). It
predicts that each mailbox word is a bitfield where each bit = one register block, and gives
the full layout. KEY POINTS for probing:

- **It is CM930, not our AT91 gateway** — strong supporting evidence, not the rulebook. Every
  bit still needs confirming on our bus. This is exactly what the sweep does.
- The map predicts **1801 (W1) bit N** selects a lower-settings block: bit 0=1000, 1=1020,
  2=1040, 3=1070, 4=1090, 5=1110, **6=1130 (the calibration block we want)**, 7=1150, …
- So on OUR bus, 1801 is a **bitmask**, not a small enum. That reframes the sweep: the values
  that matter are **powers of two** (1,2,4,8,16,…,32768), not 2,3,5,6,7. Bit 6 = value **64**
  would be the calibration (1130) trigger if the map holds.
- Our current sweep (2,3,5,6,7) tests combinations of the low bits, which is why 2 and 3
  showed nothing clean: value 2 = bit 1 (1020 block), value 3 = bits 0+1 together. Worth
  re-running the sweep over **powers of two**: 1,2,4,8,16,32,64,128,256,512,1024,2048,4096,
  8192,16384,32768 — each isolates one block. Value 64 is the calibration prize.
- W0 (1800) bit 0 = 1350 block (known). Its other bits map the calendar/clock blocks.

**Revised probe candidates (powers of two on 1801):** 1(known→1000), 2(→1020?), 4(known→1040),
8(→1070?), 16(→1090?), 32(→1110?), **64(→1130 calibration?)**, 128(→1150?), … This is a
cleaner sweep than the arbitrary 2,3,5,6,7 and directly targets each block.

---

## RESULT 20 Oct 2026 — 1801 sweep complete, all 16 bits confirmed

Run as planned (resync → sweep powers of two). Every 1801 bit maps to the predicted block at
its wire length: 1→1000, 2→1020, 4→1040, 8→1070, 16→1090, 32→1110, **64→1130 (calibration)**,
128→1150, 256→1170, 512→1190, 1024→1210, 2048→1240, 4096→1270, 8192→1290, 16384→1310,
32768→1330. No alarms, no shadow mismatches, no dropped mailbox reads. Full table in
modbus-mailbox-flags.md (UPDATE 20 Oct 2026).

Consequences:
- Delivery path now known for every lower settings register (1000–1349). Calibration (1130)
  is deliverable via 1801=64, though calibration stays read-only by user decision.
- Still open (optional): W0 / 1800 bits 1–15 (calendar 1370–1768, clock 1770, 1777, 1780).
  A sweep of 1800 could map them. Bit 15 (1780 block) cannot be served after a resync-seeded
  start, because a resync never fetches 1780–1798 (the ESP correctly declines to serve it).
- Cosmetic, for the code cleanup: Mailbox Probe Status briefly shows "sweep 17/16" at the end,
  and the "Hot Water Start (last write)" text sensor actually shows the last write OR probe
  result of any kind - rename it - DONE 20 Oct: "Last Write or Probe Result" (no "/" allowed in ESPHome names).

---

## UPDATE 21 Oct 2026 — probe and sweep tooling removed

With all 16 bits of 1801 confirmed, the "Run Mailbox Probe" and "Run 1801 Sweep" buttons, the
probe inputs, "Mailbox Probe Status", and the probe/sweep code in the component and responder
were removed. The last build containing them is thermia-gateway-2026-10-20d.tar.gz. The full
resync button and the Takeover Arm switch are unaffected.
