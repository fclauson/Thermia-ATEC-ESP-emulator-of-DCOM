# Full-table resync trigger (mailbox extreme-value pattern) — CONFIRMED

**Discovered:** 19 October 2026, from the log where the gateway was powered down and back up
**without** rebooting the heat pump. The ESP was passive (shadow mode) and captured the whole
event. This resolves the "full-table-resync trigger" that step 3 listed as unknown.

## What it is

To make the heat pump re-push its ENTIRE settings table (1000–1798) WITHOUT a reboot or a
handshake, the gateway writes an extreme-value pattern into the mailbox:

| Reg  | Normal | Resync value | Hex    |
|------|--------|--------------|--------|
| 1800 | 0      | 0            | —      |
| 1801 | 0      | 0            | —      |
| 1802 | 0      | 32767        | 0x7FFF |
| 1803 | 0      | 65535        | 0xFFFF |
| 1804 | 1919   | 128          | 0x0080 |
| 1805 | 6      | 7            | 0x0007 |

i.e. serve the mailbox as `0, 0, 32767, 65535, 128, 7` instead of the quiescent
`0, 0, 0, 0, 1919, 6`.

## What the heat pump does in response

Within ~1 s it begins re-pushing the whole 1000–1798 table as FC16 writes, block by block
(1007, 1020–1030, 1040–1052, … 1770–1779 — 34 blocks, the complete settings range). This is
the same content as the cold-boot table push, but with NO reboot and NO handshake.

## Clearing it

~34 s later (08:02:16 → 08:02:50 in the capture) the gateway writes the mailbox back to the
quiescent `0,0,0,0,1919,6` and the resync ends. So the pattern is held for the duration of
the re-push, then cleared — same "raise, then clear" shape as the 1801 flags, just a longer
hold.

## Evidence it is clean

* No handshake occurred during the resync (handshake requests = 0 throughout).
* The gateway shadow stayed at 0 mismatches the whole time — the model reproduced every
  frame of the resync correctly.
* Triggered naturally by a gateway power-cycle with the heat pump left running, which is
  exactly the "gateway reconnected with an empty table, ask for everything" case.

## Why this matters for the emulator

1. **On-demand table fill.** The strict "only serve values seen this session" rule meant a
   mid-session ESP start (gateway already gone, no boot to watch) had no way to obtain the
   settings table. Now the ESP can REQUEST a full push at any time by serving this pattern
   for ~35 s, then reverting to quiescent. This removes the main limitation of
   cold-start-only seeding.
2. **Authoritative re-sync.** If the ESP's store is ever doubted (suspected stale), it can
   force a fresh, complete copy straight from the heat pump without a reboot.
3. **Confirms the mailbox model.** Three mailbox behaviours are now confirmed on this bus:
   * 1801 = 1  → heat pump re-reads block 1000–1012
   * 1801 = 4  → heat pump re-reads block 1040–1060
   * 1802/1803/1804/1805 = 32767/65535/128/7 → heat pump re-pushes the whole 1000–1798 table
   This is a solid base for the flag-probing work (see NOTE-writable-registers-todo.md): the
   mailbox is a small command surface and we now understand three of its operations.

## Suggested use (future step, after 4b is proven)

Add an emulator action "Request full table resync": serve the extreme-value mailbox pattern,
watch the table re-push complete (last block ~1777–1779), then revert to quiescent. Use it to
seed the store on a mid-session ESP start, and as a manual "refresh everything" control. Keep
the same discipline: armed only, bus-idle checked, logged.
