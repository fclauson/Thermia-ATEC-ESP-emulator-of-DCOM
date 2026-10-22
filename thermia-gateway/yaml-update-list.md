# thermia-gateway.yaml — update list (from 21–22 Oct 2026 analysis)

**Purpose:** apply these changes to `thermia-gateway.yaml`. They come from the 21 Oct 2026 gateway log (test-mode run 17:17–17:19), the 21 Oct CHANGED report, and a second CHANGED report covering about 21 Oct 17:00 to 22 Oct 07:00. This is analysis only; nothing here has been applied.

**Revision 2 (22 Oct):** added the second report's findings in §3 (2172/2173, 2013, clock, 2091), the new §3a (2094 ODU decodes) and §3b (2082/2089/2090), and updates to §1, §6 and §7.

**Status labels:** **SEEN** = observed on our bus (count given). **INFERRED** = fits the data but not tested. Use "(unconfirmed)" in entity names for anything not SEEN at least twice.

**Do not change:** write paths, arming/takeover logic, mailbox handling, or any safety behaviour. Everything below is read-only sensors, names and comments.

---

## 1. New register entries

All `device_id: dev_mailbox`, `entity_category: diagnostic`, `accuracy_decimals: 0`, unless noted.

| Reg | Name | Settings | Evidence |
|---|---|---|---|
| 1780 | `Test Output Bits 1780 (mirror of A80C, unconfirmed)` | `signed: false` | SEEN once: 64 → 96 when the circ pump was switched on in test mode, back to 64 on exit. Same value as 2013 / A80C. |
| 1783 | `Test Pump Speed 1783 (mirror of A80F, unconfirmed)` | `unit_of_measurement: "%"` | SEEN once: 10 → 30 → 5, matching A80F / 2016 exactly. |
| 1786 | `Test ODU Fan Cmd 1786 (mirror of ABF6, unconfirmed)` | `signed: false` | SEEN once: 0 → 50 → 0 while ABF6 stepped 0,10…50 and back to 0. Only the final value appeared in 1786; scale unknown. |
| 2018 | `Reg 2018 (gateway copy of dev2 A811)` | none | SEEN once: 12 → −56 (= 200 as a byte) on test-mode entry, back to 12 on exit. The live-I/O comment already names A811 as 2018 but there is no register entry. |
| 2137 | `HP Clock Minute (2137)` | `signed: false` | INFERRED, strong (see §3) |
| 2138 | `HP Clock Hour (2138)` | `signed: false` | INFERRED, strong |
| 2139 | `HP Clock Day (2139)` | `signed: false` | SEEN twice: 20 → 21 and 21 → 22, each at midnight HP time |
| 2140 | `HP Clock Month (2140)` | `signed: false` | INFERRED from layout; never seen changing. Check the value in `/registers.json` (expect 10). |
| 2141 | `HP Clock Year (2141)` | `signed: false` | INFERRED from layout; never seen changing. Check the value (expect 26 or 2026). |
| 2142 | `HP Clock Weekday (2142)` | `signed: false` | INFERRED, strong (1 → 2 on 21 Oct, 2 → 3 on 22 Oct, both at midnight HP time; 0 = Monday, so 3 = Thursday 22 Oct) |

**Comment for 1780/1783/1786:** registers 1780–1798 (W0 bit 15 block, 0x06F4) were not seen at all until test mode was entered at 17:17:33. That moment coincided with a full settings push from the heat pump. After that, 1780/1783/1786 updated each time a test output changed. **Do not add the rest of 1781–1798 as entities.** `log_changes` and `/registers.json` already catch them, and the next test run should identify more.

**Optional (only if wanted):**

- **2086** `Reg 2086 (compressor top temp?, unconfirmed)`, `°C`. It moved 55–70 on 21 Oct. The CSV names it compressor top temperature or a second superheat value.
- **1762** `Reg 1762 (unknown, 0x06C5 block)`, `signed: false`. Values seen: 245, 251, 2. It changes only when the settings blocks are pushed (for example 22 Oct 06:45, together with 1770–1776).
- **1763** `Reg 1763 (unknown, 0x06C5 block)`, `signed: false`. 85 → 86 at the same 22 Oct 06:45 push.

## 2. Text sensor (new)

Add **"HP Clock (runtime)"** as a template text sensor in `dev_mailbox` / diagnostic. It should format 2136–2142 as `YYYY-MM-DD HH:MM:SS (weekday)`, and return `{}` until all seven registers have been seen.

Add a comment: the HP clock runs about **1 h behind local time**, so it is probably on UTC or not following summer time. Evidence (SEEN on two days): 2139/2142 rolled over at 01:00 local on 21 and 22 Oct; 1770–1772 read 13:31 when local time was about 14:30 (21 Oct) and 05:44:56 at the 06:45 push (22 Oct).

## 3. Renames and comment changes to existing entries

**2136**
- Rename `Post Box 2136 (unconfirmed)` → `HP Clock Second (2136)`.
- Replace the "suspected post box" comment with: runtime copy of the HP clock, 2136 = seconds … 2142 = weekday, the same layout as CSV 1770–1776 (system time). The +21 jumps seen earlier are just the ~21 s runtime rotation sampling a seconds field.

**2013** — add to the comment:
- In test mode, 1780 mirrors this value.
- On 21 Oct the value reached 97 (bits 6 + 5 + 0) during a hot-water run (1363 = 4, Hot Water). This fits bit 0 = change-over valve to hot water. Keep the existing bit 0/5/6 notes.
- On 22 Oct it sat at **65** (bits 6 + 0) while idle in Hot Water mode, and showed 97 with the pump on. So the valve stays on the hot-water side between runs; 64 only appears when the valve is on the heating side.

**2016** — add to the comment: 1783 mirrors this in test mode. In normal running it modulates over 10–100 % (30 distinct values on 21 Oct).

**2034**
- Rename to `Reg 2034 (HP cmd to dev4 ABF6, ODU fan cmd?)`.
- Comment: the test-mode fan ramp drove ABF6 0→10→20→30→40→50 (SEEN once). Normal running reached 80. 1786 mirrors it in test mode.

**2032 / 2036**
- Rename 2036 to `Reg 2036 (same as 2032 / ABF4)`.
- Comment on both: identical values, only 4 distinct (4–78). It kept changing until 16:55 while the HP was idle after 13:20, so it is **not** compressor demand. Meaning unknown.

**2029** — keep the name. Add a comment: it drifts smoothly between 17 and 24 (21–22 Oct) along with dev4 ABE9, so it behaves like a temperature, not an error code (CM930 calls it an outdoor-unit error code).

**2024** — keep the CSV name. Add a comment: it tracks live dev4 ABE4 (3–8 on 21–22 Oct) and changes all day, so it behaves like a live defrost or coil sensor temperature, not a calibration offset.

**2172 / 2173** — replace "Confirm at the next defrost". Three defrosts have now been seen:

| Defrost (local time) | 2171 | 2172 | 2173 |
|---|---|---|---|
| 21 Oct ~09:25 | 3122 → 3123 | 422 → **18417** | had reached 18415, reset to 0, rose to 109 by 13:15 |
| Between 21 Oct 17:00 and 22 Oct 04:20 (exact time not in the summary) | 3123 → 3124 | new value not in the summary | presumably reset to 0 (2173 max in this window was 226) |
| 22 Oct ~04:20 | 3124 → 3125 | → **160** | reset to 0, rose to 147 by 07:00 while compressor hours (2160) rose |

This is now SEEN three times and supports:
- **2173 = compressor run minutes since the last defrost.** It rises only while the compressor runs and resets at each defrost.
- **2172 = that count captured at the defrost**, i.e. compressor minutes between the last two defrosts. 18417 min ≈ 307 h since the April defrost; 160 min between two cold-night defrosts.

Changes:
- Rename 2172 to `Compressor Minutes Between Last 2 Defrosts (2172)` and 2173 to `Compressor Minutes Since Last Defrost (2173)`, or keep the CSV names and put this in the comments.
- Remove "probable" from both comments. `unit_of_measurement: "min"` stays.
- One check is still open (no YAML change): in the live log, confirm that 2173 read about 160 just before the 22 Oct 04:20 reset.

**2091 "ODU Fan 1 Speed"** — rename to `Reg 2091 (defrost-related flag?, unconfirmed)` and move it to `dev_mailbox` / diagnostic. Comment:
- It only ever reads 0 or 1, while the real fan command (2034 / ABF6) ranges 0–80.
- It toggles together with **2132** and **2148** a few minutes after defrosts (21 Oct 09:35, 22 Oct 04:25). 4 pulses against 2–3 defrosts, so not exactly one per defrost.
- Do not add 2132 / 2148 as entities yet; `log_changes` catches them.

**Test Mode Active (unconfirmed)** — update the comment to "SEEN in two test-mode runs" (16 Oct and 21 Oct 17:17:33–17:18:42, A811 12 → 200 → 12). Keep "(unconfirmed)" in the name for now.

**2032 / 2036 (addition)** — on 22 Oct it took a new value, 6, at 04:25, just after the defrost. Add "may be partly defrost-related" to the comment.

**2010 / 2035 / 2055** — not registered, and no need to add them. If anywhere lists them, note that they are copies of outdoor temp 2023.

## 3a. ODU status decodes from 2094 — validity warning (comments only)

Over about 14 h on 21–22 Oct, with the compressor running and at least two defrosts, **2094 never changed**. Neither did 2092, 2093 or 2095. A defrost normally reverses the four-way valve, so "ODU Fourway Valve Status" should have changed.

- Strengthen the existing "VALIDITY TO CHECK" comment above 2091–2096 with this evidence.
- Add "(unverified)" to the names of the binary sensors decoded from 2094: "ODU Fourway Valve Status", "ODU Fan Status", "ODU Flow Detected", and any other 2094 bit decode.
- Do not delete them; they may still be right on a different firmware or profile.
- Also add "(unverified)" to 2092 "ODU Fan 2 Speed", 2093 "ODU Expansion Valve Step" and 2095 "ODU Operation Status". 2096 "ODU High Pressure Transmitter" did change (30–47, around the 04:20 defrost), so leave it.

## 3b. 2082 / 2089 / 2090 — they switch together (comments only)

These three always change at the same moment between two states:

| State | 2082 | 2089 | 2090 |
|---|---|---|---|
| A | 45060 | 0 | 0 |
| B | 56325 | 256 | 1 |

- State A from 21 Oct 13:20 (end of a run). State B from 22 Oct 04:00 onwards.
- Only 4 changes in ~14 h despite many compressor cycles, so it is **not** simple compressor on/off.
- 2089 = 256 and 2090 = 1 look like the same flag in different bytes.
- Add one shared comment above these entries saying so. No renames yet.

## 4. Live-I/O section header comment (add)

Add this mapping, which explains most `Reg 20xx (copy of …)` entries:

```text
Runtime 2000-2039 = gateway image of the internal live bus
dev2 reads   43000+n -> 2000+n   (A7F8->2000 supply, A7F9->2001 return,
                                  A7FB->2003, A7FC->2004, A802->2010 outdoor)
dev2 cmds    A80C->2013, A80E->2015, A80F->2016, A811->2018
dev4 reads   44000+n -> 2020+n   (ABE0->2020, ABE1->2021, ABE2->2022,
                                  ABE4->2024, ABE9->2029, ABEA->2030)
dev4 cmds    ABF4->2032 (2036 identical), ABF6->2034
dev6 cmds    AFDC->2074, AFDD->2075, AFE0->2078  (consistent with existing entries)
Outdoor temp is also sent packed as 0x05nn (low byte = outdoor temp, high byte = 5, unknown)
to dev04 ABF7, dev05 AC29 and dev06 AFE0 (= 2078).
The runtime copies lag the live values by up to ~21 s, so snapshot min/max can differ slightly
(e.g. 2021 max 23 vs ABE1 max 17 on 22 Oct).
```

## 5. Signed-value checks (verify, change only if wrong)

These registers have gone below zero: 2020 (−1 on 21 Oct, −2 on 22 Oct), 2085 (−3.1 °C, then −3.2 °C), 2087 (−3.1 K on 21 Oct) and 2120 (−493, then −139). Their entries omit `signed:`. The CHANGED report's "last" column shows them raw (e.g. 2085 = 65509, 2020 = 65534) while smin shows them signed.
- Confirm the component's default is signed, so that they display as negative and not as 6553x.
- If the default is unsigned, add `signed: true` to those four.
- **2082** is `signed: false` and showed 45060 / 56325 (−20476 / −9211 if signed). Meaning unknown, so leave it as is.

## 6. CHANGED report (only if it is generated by this YAML or its tooling)

The report appears to sample about every 5 minutes. 2136 and 2137 both show 204 changes over about 17 h. Short events, such as a 25 s test-mode step, can be missed and "changes" undercounts.
- Add a note to the report header saying so.
- The 22 Oct report was described as "3 days" but covers about 14 h (165 changes of 2136; 2139 only shows days 21 and 22). It may reset on ESP restart. Add the report's start time to the header.
- Show "last" with the same signed handling as smin/smax.
- If cheap, record changes from `log_changes` events instead of snapshots.

## 7. Open items (no YAML change, for reference)

- **1786 scale,** and whether 1780–1798 follows each step or only updates now and then.
- **The other test outputs** (four-way valve, aux/immersion heater, compressor). These should name more of 1781–1798.
- **What triggered the 17:17:33 full settings push:** test-mode entry, or something else.
- **ABF4 (2032/2036) and ABEA's other bits.**
- **1762 / 1763 and 2083–2084:** meaning unknown.
- **2082 / 2089 / 2090 state (§3b):** what switches them.
- **2091 / 2132 / 2148 (§3):** what exactly they flag around a defrost.
- **2094 and the other ODU words (§3a):** find which registers really carry four-way valve, fan and flow status. The next defrost is a good test.
- **2173 just before the 22 Oct 04:20 reset:** confirm it was about 160.
