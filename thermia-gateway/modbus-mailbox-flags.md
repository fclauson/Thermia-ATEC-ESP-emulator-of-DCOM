# Modbus mailbox flags (1800–1805) — handoff

**Purpose:** paste or attach this at the start of a new chat, alongside `HANDOFF.md`. It explains what the mailbox registers 1800–1805 mean, based on the CM930 `modbus-flags.json` map, and how well that map matches our own bus captures. Written 19 October 2026. Analysis only: no firmware was changed.

**Sources used**

- `CM930_MODBUS_FLAGS_MAP.md`: a write-up of `/etc/device-client.d/930/modbus-flags.json` from **Thermia Connect 2.1.105** (CM930), dated 5 Oct 2026.
- `HANDOFF.md` §4, §6b and §10 (the mailbox events and the 1053 method).
- Capture `thermia_capture_20261004_134926 - cold start up.log`, decoded by hand for this note.

**Important caveat:** CM930 is the newer Thermia Connect gateway, **not our AT91SAM7X256 Thermia Online gateway**. The way registers are grouped into blocks matches our bus exactly, but some behaviour differs (see §4). Treat the JSON as strong supporting evidence, not as the rulebook for our gateway.

---

## UPDATE 20 Oct 2026 — W1 (1801) CONFIRMED on our bus, all 16 bits

Powers-of-two sweep run from the ESP as sole gateway (06:29:55–06:36:41, after a full resync
seeded the store). Every bit made the heat pump FC03-read exactly the predicted block, at the
**wire** length, and the ESP served each block unchanged from its store:

| 1801 value | Bit | Block read | Length | Result |
|---:|---:|---|---:|---|
| 1 | 0 | 1000 | 13 | known, re-confirmed |
| 2 | 1 | 1020 | 11 | **NEW, confirmed** |
| 4 | 2 | 1040 | 21 | known, re-confirmed |
| 8 | 3 | 1070 | 15 | **NEW, confirmed** (Heat Pump Control) |
| 16 | 4 | 1090 | 13 | **NEW, confirmed** (Cooling) |
| 32 | 5 | 1110 | 12 | **NEW, confirmed** (Immersion) |
| 64 | 6 | 1130 | 18 | **NEW, confirmed** (Calibration) |
| 128 | 7 | 1150 | 19 | **NEW, confirmed** |
| 256 | 8 | 1170 | 9 | **NEW, confirmed** |
| 512 | 9 | 1190 | 13 | **NEW, confirmed** |
| 1024 | 10 | 1210 | 22 | **NEW, confirmed** |
| 2048 | 11 | 1240 | 27 | **NEW, confirmed** |
| 4096 | 12 | 1270 | 14 | **NEW, confirmed** |
| 8192 | 13 | 1290 | 19 | **NEW, confirmed** |
| 16384 | 14 | 1310 | 9 | **NEW, confirmed** |
| 32768 | 15 | 1330 | 18 | **NEW, confirmed** |

Health throughout: Active Alarm Bits = None on every update, Gateway Shadow Mismatches 0,
Mailbox Reads Dropped 0, Seconds Since HP Update 0–2 s, bad frames 2. One harmless
"took 70 ms" component warning (the known large-reply TX time).

So W1 is a per-block bitmask exactly as the CM930 file describes, and **every lower settings
block 1000–1349 now has a proven delivery flag**. W1 alone is enough on our bus (no W3 bit
needed). W0 (1800) is still only confirmed for bit 0 (1350); bits 1–15 are predicted.

Also confirmed 20 Oct (resync log): a full resync (1802/1803/1804/1805 = 7FFF/FFFF/0080/0007)
makes the heat pump push 31 settings blocks — all 16 W1 blocks and W0 blocks 0546…06F1 — but
**not 06F4 (1780–1798, W0 bit 15)**, matching 1802 = 0x7FFF leaving bit 15 clear. The ESP
therefore never holds 1780–1798 after a resync-seeded start. During the resync hold the heat
pump also pushes 085F (2143–2147) on every poll, matching 1804 = 0x0080 (W4 bit 7).

**Our ESP's use of the request words (22 Oct 2026):** the ESP sets 1802/1803 (upload requests)
ONLY during a full resync (0 0 7FFF FFFF 0080 0007, 35 s). Settings writes raise 1800/1801
bits only. From 22 Oct the component logs every non-steady mailbox reply and classes every
settings push from the heat pump as requested / UNSOLICITED (README-build-and-run.md, event log
lines), to settle whether the heat pump ever pushes on its own (e.g. the 06F4 block at
test-mode entry on 21 Oct).

---

## 1. The model in one paragraph

Each mailbox word is a **16-bit field where each bit stands for one block of registers**. The HP polls 1800–1805 every ~4.2 s, then acts on whichever bits are set:

| Word | Reg | Hex | Role (CM930 naming) | What the HP does on our bus |
|---|---:|---|---|---|
| W0 | 1800 | 0708 | desired/alert, **upper** settings (1350–1798) | FC03 **reads** the block from the gateway |
| W1 | 1801 | 0709 | desired/alert, **lower** settings (1000–1349) | FC03 **reads** the block from the gateway |
| W2 | 1802 | 070A | read/current request, upper settings | FC16 **writes** (uploads) the block to the gateway *(inferred)* |
| W3 | 1803 | 070B | read/current request, lower settings | FC16 **writes** (uploads) the block to the gateway *(inferred)* |
| W4 | 1804 | 070C | runtime request (2000–2239) | FC16 writes the requested runtime blocks: this is the ~21 s live rotation |
| W5 | 1805 | 070D | **not in the JSON** | always reads **6** on our bus, meaning unknown |

W0/W2 share one bit layout and W1/W3 share another. The *upper* settings live in the *lower-numbered* word (W0/W2), so the words are not in address order.

Steady-state mailbox reply on our bus (every poll in the 4 Oct capture):

```text
0f 03 0c  0000 0000 0000 0000 077f 0006
          W0   W1   W2   W3   W4   W5
```

---

## 2. Bit maps, with the lengths we actually see on the wire

"File span" is the address range the JSON gives each bit. "Wire" is the register count the HP actually sends on our bus (boot push, or live rotation for W4). **Do not take wire counts from the file**: four blocks differ (marked ◆).

### W1 / W3 — lower settings (1801 / 1803)

| Bit | Start dec | Start hex | File span | Wire | Notes |
|---:|---:|---|---:|---:|---|
| 0 | 1000 | 03E8 | 20 | **13** ◆ | 1801=1 → HP re-reads 1000–1012 (observed). Heating curve 1000, room setpoint 1012 |
| 1 | 1020 | 03FC | 20 | **11** ◆ | |
| 2 | 1040 | 0410 | 21 | 21 | 1801=4 → HP re-reads 1040–1060 (observed). 1053 HW start, 1055 Legionella interval |
| 3 | 1070 | 042E | 15 | 15 | |
| 4 | 1090 | 0442 | 13 | 13 | |
| 5 | 1110 | 0456 | 12 | 12 | |
| 6 | 1130 | 046A | 18 | 18 | |
| 7 | 1150 | 047E | 19 | 19 | |
| 8 | 1170 | 0492 | 9 | 9 | |
| 9 | 1190 | 04A6 | 13 | 13 | |
| 10 | 1210 | 04BA | 22 | 22 | |
| 11 | 1240 | 04D8 | 27 | 27 | |
| 12 | 1270 | 04F6 | 14 | 14 | |
| 13 | 1290 | 050A | 19 | 19 | |
| 14 | 1310 | 051E | 9 | 9 | |
| 15 | 1330 | 0532 | 20 | **18** ◆ | |

### W0 / W2 — upper settings (1800 / 1802)

| Bit | Start dec | Start hex | File span | Wire | Notes |
|---:|---:|---|---:|---:|---|
| 0 | 1350 | 0546 | 20 | 20 | 1800=1 → HP re-reads 1350–1369 (observed twice, content unchanged). Contains 1363 operation mode |
| 1 | 1370 | 055A | 33 | 33 | Calendar group. Locally proven on the XTR controller (see CM930 note) |
| 2 | 1403 | 057B | 33 | 33 | |
| 3 | 1436 | 059C | 33 | 33 | Calendar group. **1436 is one of the two writes the HP retries forever when there is no gateway** |
| 4–12 | 1469…1733 | 05BD…06C5 | 33 each | 33 each | 05BD 05DE 05FF 0620 0641 0662 0683 06A4 06C5 |
| 13 | 1770 | 06EA | 7 | 7 | HP's own clock (1770–1776) |
| 14 | 1777 | 06F1 | 3 | 3 | Boot value FFFF FFFF FFFF |
| 15 | 1780 | 06F4 | 19 | 19 | |

### W4 — runtime (1804)

| Bit | Start dec | Start hex | File span | Wire | In live rotation? | Notes |
|---:|---:|---|---:|---:|---|---|
| 0 | 2000 | 07D0 | 19 | 19 | yes | |
| 1 | 2020 | 07E4 | 17 | 17 | yes | |
| 2 | 2040 | 07F8 | 17 | 17 | yes | |
| 3 | 2060 | 080C | 18 | 18 | yes | |
| 4 | 2080 | 0820 | 18 | 18 | yes | |
| 5 | 2100 | 0834 | 18 | 18 | yes | |
| 6 | 2120 | 0848 | 23 | 23 | yes | |
| 7 | 2143 | 085F | 5 | 5 | **no**: sent once at boot | **2143 is the other write the HP retries forever with no gateway** |
| 8 | 2148 | 0864 | 12 | **4** ◆ | yes | CM930 read-all *excludes* this bit; ours requests it |
| 9 | 2160 | 0870 | 17 | 17 | yes | Running-hour counters 2160–2168 |
| 10 | 2180 | 0884 | 60 | 60 | yes | |

---

## 3. What the map explains (checked against our bus)

1. **All three mailbox events in HANDOFF §4 decode exactly.** 1801=1 is W1 bit 0 (1000 block). 1801=4 is W1 bit 2 (1040–1060, exact). 1800=1 is W0 bit 0 (1350–1369, exact).
2. **The startup settings push is the full bit map.** In the 4 Oct cold-start capture the HP writes, in order:
   - all 16 W1/W3 blocks (03E8 → 0532, from 44.762 s),
   - then all 16 W0/W2 blocks (0546 → 06F4, ending 76.921 s),
   - then **085F x5** (W4 bit 7) at 76.983 s.

   That is **33 writes**, and the wire lengths add up to **exactly 698 registers**, matching the counts already in HANDOFF §4. The first mailbox poll follows at 80.387 s and the live rotation starts at 81.121 s.
3. **1804 = 1919 is a request, not a mystery value.** 1919 = 0x077F = bits 0–6 and 8–10, which are exactly the 10 runtime blocks in the ~21 s live rotation. Bit 7 (085F) is the only runtime block not requested, and it is the only one not in the rotation. So the gateway asks and the HP serves.
4. **The "reconnect signature" (HANDOFF §4) is a full upload request.** Read as bit fields, 1802=32767 / 1803=65535 / 1804≈128 means: all lower settings blocks, all but one upper block, and W4 bit 7 (085F). That is essentially the same set as the 33-write boot push, which is what follows it. Two caveats: the recorded W4 figure was "~128", and 32767 leaves out bit 15 (06F4). Both need checking against the raw capture.
5. **Why 1061 (hot water stop temp) is never seen.** 1061 = 0x0425 falls in a gap the file leaves out of every block (0425–042D). The 0410 block ends at 1060.
6. **The two writes the HP retries forever with no gateway (1436, 2143)** are both block start addresses: W0/W2 bit 3 (059C) and W4 bit 7 (085F).

## 4. Where our gateway differs from CM930

| Point | CM930 (file) | Our bus |
|---|---|---|
| Runtime block left out of normal requests | bit 8 (0864) | **bit 7 (085F)**; bit 8 is requested every cycle |
| 1805 / W5 | no mapping | **constant 6** |
| Desired transaction | XTR needed **W0 bitN + W2 bitN together** | 1801=4 **alone** triggered the 1040–1060 read (1803 not seen set, going by our notes) |
| Block lengths | file spans | 03E8=13, 03FC=11, 0532=18, 0864=4 (shorter than file) |

The file's own warnings apply too: don't take wire counts from it, and don't fold the gaps into neighbouring blocks.

Unmapped gaps in the settings space: 0425–042D, 043D–0441, 044F–0455, 0462–0469, 047C–047D, 0491, 049B–04A5, 04B3–04B9, 04D0–04D7, 04F3–04F5, 0504–0509, 051D, 0527–0531, 06E6–06E9, 0707.
Runtime gaps: 07E3, 07F5–07F7, 0809–080B, 081E–081F, 0832–0833, 0846–0847, 0881–0883.

---

## 5. What this changes for the project

**Helps:**

- **The 1053 method (HANDOFF §10.0c) works for any settings register, not just one.** The method is: write the target register to the gateway with FC06, then make an online change to *any* online-editable register in the **same block**. The gateway raises that block's W0/W1 bit and the HP re-reads the whole block, including our value. The bit map tells us which block each register belongs to, and therefore which online-editable "trigger" register to use.
- **It suggests a selective resync instead of a full one (HANDOFF §10 step 3).** In principle, a single W2/W3 bit asks the HP to upload just that one block, which would let us read the HP's own current value of any setting. Untested.
- **It is most of the spec for a future local gateway or relay** answering 1800–1805 itself: which bits to raise, what the HP will do, and what to expect at boot (33-page push) and in steady state (W4=0x077F, W5=6).

**Doesn't help:**

- **We still can't get our own flag seen by the HP.** The gateway rebuilds the mailbox when the HP polls it, so external flag writes read 0 at the HP's poll (HANDOFF §6b, §10.0c). The map says *what* to write, not *how* to get it in front of the HP.
- **The handshake (HANDOFF §5) is unaffected.**

**Safety:** knowing a bit's mapping is not permission to use it. Keep the existing rules: explicit button presses only, bus-idle checks, read-backs, and a known way to roll back. Only use a W0/W1 bit for a write once that bit has been proven on our bus.

---

## 6. Open items and tests, in rough priority

1. **Join the block map with `thermia-register-definitions.csv` / `cm930_semantic_objects.csv`.** For each block, list its named registers and which are editable online. That gives a trigger register per block for the generalised 1053 method, and should explain what 1350–1369 is for.
2. **Check the reconnect signature against the raw capture.** Get the exact W2/W3/W4 values and see whether the resync upload is exactly the requested blocks (is 06F4 skipped? is the W4 value exactly 0x0080?).
3. **Test the "HP pushes on change" hypothesis.** Are 059C (1436) and 085F (2143) blocks the HP sends unprompted when something changes on its own side? Check `thermia_capture_20260924_210001 at HP change of value…`: does an HP-side change produce an unsolicited FC16 to one block with no mailbox bit set first?
4. **Work out what W5 / 1805 = 6 means.** Does it ever change (boot, gateway power-cycle, online change)? It may be a version, capability or status word.
5. **Confirm whether W0/W1 alone are enough on our bus.** Check the 1801=4 events for any non-zero 1803 at the same poll.
6. **Explain why our gateway leaves out bit 7 while CM930 leaves out bit 8.** It is consistent with 085F being event-driven on our HP, but that is not proven.
7. **Compare against Connect firmware 2.0.18** if it is ever recovered.

## 7. Status labels used

- **PROVEN on our bus:** §3 items 1, 2, 3, 5, the W4/W5 steady-state values, **all 16 W1 bits
  (sweep, 20 Oct 2026)**, W0 bit 0, and the resync skipping 06F4.
- **Firmware-derived (CM930), consistent with our bus:** the bit layouts in §2 and the W0–W4 role names.
- **Inferred / to test:** W2/W3 meaning "HP uploads the block"; the reconnect-signature decode (§3 item 4); W5 meaning; HP-pushes-on-change (§6 item 3); selective resync.
