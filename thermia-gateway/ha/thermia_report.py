#!/usr/bin/env python3
"""thermia_report.py  v2026.10.22.2  - which registers changed, and which stayed fixed.

Run on your PC over the daily files written by thermia_collector.py (copy the folder
/config/thermia_log from Home Assistant, e.g. via the Samba share). Python 3.8+, standard
library only.

  python thermia_report.py thermia_log/
  python thermia_report.py thermia_log/ --names thermia-gateway.yaml --csv report.csv
  python thermia_report.py thermia_log/ --since 2026-11-01 --only-unmapped
  python thermia_report.py thermia_log/ --events          # also print every event, in time order

For every key (registers as "1000", live I/O as "dev02.w.10") it reports:
  changes   number of times the value changed (0 = fixed over the whole period)
  distinct  number of different values seen
  min/max   raw unsigned 16-bit, and as signed int16 (smin/smax)
  last      raw, and as signed int16 (slast)
  first value, time of the last change

"Changed" registers are listed first (most changes first), then "fixed" ones with their value.
dev4 reads that only ever show 64536 (= -1000, sensor not fitted) are listed separately.

EVENTS: exact-time changes of the ESP's watchlist (defrost, ODU run mode, test-mode block
1780-1798, ...), captured between samples. Each day file is also checked to contain that day's
data, and identical day files are flagged.

Sampling: the collector reads the ESP every 5 minutes, so a value that changes and changes
back between two reads (e.g. a 25 s test-mode step) is not seen, and "changes" counts changes
between samples, not every change on the bus. For short events use the ESP's log_changes
output (ESPHome log, DEBUG) instead.
"""

import argparse
import csv
import hashlib
import datetime as dt
import glob
import gzip
import json
import os
import re
import sys


def s16(v):
    return v - 65536 if v >= 32768 else v


def key_sort(k):
    return (0, int(k), "") if k.isdigit() else (1, 0, k)


def load_names(yaml_path):
    """register number -> name, from the '- register: N' / 'name: "..."' pairs in the YAML."""
    names = {}
    try:
        text = open(yaml_path, encoding="utf-8").read()
    except OSError as e:
        sys.exit(f"cannot read {yaml_path}: {e}")
    for m in re.finditer(r'-\s*register:\s*(\d+)\s*\n\s*name:\s*"([^"]*)"', text):
        names[m.group(1)] = m.group(2)
    return names


def read_records(log_dir, since, warnings):
    files = sorted(glob.glob(os.path.join(log_dir, "thermia-????-??-??.jsonl.gz")))
    if since:
        files = [f for f in files if os.path.basename(f)[8:18] >= since]
    if not files:
        sys.exit(f"no thermia-YYYY-MM-DD.jsonl.gz files in {log_dir}")
    hashes = {}
    for path in files:
        name, day = os.path.basename(path), os.path.basename(path)[8:18]
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if digest in hashes:
            warnings.append(f"{name} is IDENTICAL to {hashes[digest]} (wrong file exported or copied twice?)")
        hashes[digest] = name
        wrong_day = 0
        try:
            with gzip.open(path, "rt", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    rec = json.loads(line)
                    if rec.get("type") in ("snapshot", "delta") and fmt_t(rec.get("t", 0))[:10] != day:
                        wrong_day += 1
                    yield rec
        except (OSError, EOFError, ValueError) as e:
            warnings.append(f"{name} damaged after its last good record ({e})")
        if wrong_day:
            warnings.append(f"{name}: {wrong_day} record(s) are not from {day} (file named for the wrong day?)")


def fmt_t(t):
    return dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M") if t else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_dir")
    ap.add_argument("--names", help="thermia-gateway.yaml, to show register names and spot unmapped ones")
    ap.add_argument("--since", help="only files from this day on (YYYY-MM-DD)")
    ap.add_argument("--only-unmapped", action="store_true", help="hide registers that are in the YAML (needs --names)")
    ap.add_argument("--csv", help="also write the full table to this CSV file")
    ap.add_argument("--events", action="store_true", help="also print every event row in time order")
    a = ap.parse_args()
    if a.only_unmapped and not a.names:
        sys.exit("--only-unmapped needs --names thermia-gateway.yaml")
    names = load_names(a.names) if a.names else {}

    stats = {}
    t_first = t_last = None
    snapshots = restarts = deltas = gaps = 0
    days = set()
    warnings, events, uptime = [], [], None
    for rec in read_records(a.log_dir, a.since, warnings):
        t = rec.get("t", 0)
        if t:
            t_first = t if t_first is None else min(t_first, t)
            t_last = t if t_last is None else max(t_last, t)
            days.add(fmt_t(t)[:10])
        typ = rec.get("type")
        if typ == "event":
            events.append(rec)
            continue
        if typ == "event_gap":
            gaps += rec.get("missed", 0)
            continue
        if "uptime_s" in rec:
            uptime = rec["uptime_s"]
        if typ == "snapshot":
            snapshots += 1
        else:
            deltas += 1
        if rec.get("restart"):
            restarts += 1
        for k, v in rec.get("v", {}).items():
            s = stats.get(k)
            if s is None:
                stats[k] = {"first": v, "last": v, "min": v, "max": v, "smin": s16(v), "smax": s16(v),
                            "changes": 0, "values": {v}, "last_change": None}
                continue
            if v != s["last"]:  # daily snapshots repeat unchanged values: not a change
                s["changes"] += 1
                s["last_change"] = t
                s["last"] = v
            s["values"].add(v)
            s["min"], s["max"] = min(s["min"], v), max(s["max"], v)
            s["smin"], s["smax"] = min(s["smin"], s16(v)), max(s["smax"], s16(v))

    keys = sorted(stats, key=key_sort)
    not_fitted = [k for k in keys if k.startswith("dev04.r.") and stats[k]["values"] == {64536}]
    keys = [k for k in keys if k not in not_fitted]
    if a.only_unmapped:
        keys = [k for k in keys if k not in names]
    changed = sorted([k for k in keys if stats[k]["changes"] > 0], key=lambda k: (-stats[k]["changes"], key_sort(k)))
    fixed = [k for k in keys if stats[k]["changes"] == 0]

    hours = (t_last - t_first) / 3600 if t_first and t_last else 0
    print(f"Data from {fmt_t(t_first)} to {fmt_t(t_last)}  ({hours:.1f} h, {len(days)} calendar day(s))")
    up = f"{uptime / 3600:.1f} h" if uptime is not None else "unknown"
    print(f"Records: {snapshots} snapshot(s), {deltas} delta(s), {len(events)} event(s), ESP restarts: {restarts}, "
          f"ESP uptime at the end: {up}")
    if gaps:
        print(f"Events missed (ESP buffer overflowed between samples): {gaps}")
    for w in warnings:
        print(f"WARNING: {w}")
    print(f"Keys: {len(keys)} reported - {len(changed)} changed, {len(fixed)} fixed")
    print("Note: values are sampled every 5 min; changes that revert between samples are not seen,")
    print("      so 'changes' can undercount (use the ESP log_changes output for short events).\n")

    def name_of(k):
        return names.get(k, "") if k.isdigit() else "(live I/O)"

    hdr = (f"{'key':<14}{'changes':>8}{'distinct':>9}{'min':>7}{'max':>7}{'smin':>8}{'smax':>7}{'last':>7}"
           f"{'slast':>8}  {'last change':<17} name")
    print("CHANGED")
    print(hdr)
    for k in changed:
        s = stats[k]
        print(f"{k:<14}{s['changes']:>8}{len(s['values']):>9}{s['min']:>7}{s['max']:>7}{s['smin']:>8}{s['smax']:>7}"
              f"{s['last']:>7}{s16(s['last']):>8}  {fmt_t(s['last_change']):<17} {name_of(k)}")
    if not_fitted:
        print(f"\nNOT FITTED (always 64536 = -1000): {', '.join(not_fitted)}")
    print("\nFIXED (never changed)")
    print(f"{'key':<14}{'raw':>7}{'signed':>8}  name")
    for k in fixed:
        s = stats[k]
        print(f"{k:<14}{s['last']:>7}{s16(s['last']):>8}  {name_of(k)}")

    if events:
        by_key = {}
        for e in events:
            by_key.setdefault(e["k"], []).append(e)
        print("\nEVENTS (exact-time changes between samples, ESP watchlist)")
        print(f"{'key':<16}{'events':>7}  {'values (in order, max 10)':<44} {'last event':<17} name")
        for k in sorted(by_key, key=key_sort):
            ev = sorted(by_key[k], key=lambda e: e["t"])
            vals = " ".join(str(e["val"]) for e in ev[:10]) + (" ..." if len(ev) > 10 else "")
            print(f"{k:<16}{len(ev):>7}  {vals:<44} {fmt_t(ev[-1]['t']):<17} {name_of(k)}")
        if a.events:
            print("\nEVENT TIMELINE")
            for e in sorted(events, key=lambda e: e["t"]):
                ts = dt.datetime.fromtimestamp(e["t"]).strftime("%Y-%m-%d %H:%M:%S")
                print(f"{ts}  {e['k']:<16} = {e['val']:>6} ({s16(e['val'])})  {name_of(e['k'])}")

    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["key", "status", "changes", "distinct", "min_raw", "max_raw", "min_signed", "max_signed",
                        "first_raw", "last_raw", "last_signed", "last_change", "name"])
            for k in keys:
                s = stats[k]
                w.writerow([k, "changed" if s["changes"] else "fixed", s["changes"], len(s["values"]), s["min"],
                            s["max"], s["smin"], s["smax"], s["first"], s["last"], s16(s["last"]),
                            fmt_t(s["last_change"]) if s["last_change"] else "", name_of(k)])
        print(f"\nCSV written: {a.csv}")


if __name__ == "__main__":
    main()
