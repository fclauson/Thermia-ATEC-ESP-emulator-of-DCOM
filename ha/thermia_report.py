#!/usr/bin/env python3
"""thermia_report.py  v2026.10.20.3  - which registers changed, and which stayed fixed.

Run on your PC over the daily files written by thermia_collector.py (copy the folder
/config/thermia_log from Home Assistant, e.g. via the Samba share). Python 3.8+, standard
library only.

  python thermia_report.py thermia_log/
  python thermia_report.py thermia_log/ --names thermia-sniffer.yaml --csv report.csv
  python thermia_report.py thermia_log/ --since 2026-11-01 --only-unmapped

For every key (registers as "1000", live I/O as "dev02.w.10") it reports:
  changes   number of times the value changed (0 = fixed over the whole period)
  distinct  number of different values seen
  min/max   raw unsigned 16-bit, and as signed int16
  first, last, last change time

"Changed" registers are listed first (most changes first), then "fixed" ones with their value.
"""

import argparse
import csv
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


def read_records(log_dir, since):
    files = sorted(glob.glob(os.path.join(log_dir, "thermia-????-??-??.jsonl.gz")))
    if since:
        files = [f for f in files if os.path.basename(f)[8:18] >= since]
    if not files:
        sys.exit(f"no thermia-YYYY-MM-DD.jsonl.gz files in {log_dir}")
    for path in files:
        try:
            with gzip.open(path, "rt", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        yield json.loads(line)
        except (OSError, EOFError, ValueError) as e:
            print(f"warning: {os.path.basename(path)} damaged after its last good record ({e})", file=sys.stderr)
    return


def fmt_t(t):
    return dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M") if t else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_dir")
    ap.add_argument("--names", help="thermia-sniffer.yaml, to show register names and spot unmapped ones")
    ap.add_argument("--since", help="only files from this day on (YYYY-MM-DD)")
    ap.add_argument("--only-unmapped", action="store_true", help="hide registers that are in the YAML (needs --names)")
    ap.add_argument("--csv", help="also write the full table to this CSV file")
    a = ap.parse_args()
    if a.only_unmapped and not a.names:
        sys.exit("--only-unmapped needs --names thermia-sniffer.yaml")
    names = load_names(a.names) if a.names else {}

    stats = {}
    t_first = t_last = None
    snapshots = restarts = deltas = 0
    for rec in read_records(a.log_dir, a.since):
        t = rec.get("t", 0)
        t_first = t if t_first is None else t_first
        t_last = t
        if rec.get("type") == "snapshot":
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
    if a.only_unmapped:
        keys = [k for k in keys if k not in names]
    changed = sorted([k for k in keys if stats[k]["changes"] > 0], key=lambda k: (-stats[k]["changes"], key_sort(k)))
    fixed = [k for k in keys if stats[k]["changes"] == 0]

    print(f"Period: {fmt_t(t_first)} .. {fmt_t(t_last)}   records: {snapshots} snapshot(s), {deltas} delta(s), "
          f"ESP restarts: {restarts}")
    print(f"Keys: {len(keys)} reported - {len(changed)} changed, {len(fixed)} fixed\n")

    def name_of(k):
        return names.get(k, "") if k.isdigit() else "(live I/O)"

    hdr = f"{'key':<14}{'changes':>8}{'distinct':>9}{'min':>7}{'max':>7}{'smin':>8}{'smax':>7}{'last':>7}  {'last change':<17} name"
    print("CHANGED")
    print(hdr)
    for k in changed:
        s = stats[k]
        print(f"{k:<14}{s['changes']:>8}{len(s['values']):>9}{s['min']:>7}{s['max']:>7}{s['smin']:>8}{s['smax']:>7}"
              f"{s['last']:>7}  {fmt_t(s['last_change']):<17} {name_of(k)}")
    print("\nFIXED (never changed)")
    print(f"{'key':<14}{'raw':>7}{'signed':>8}  name")
    for k in fixed:
        s = stats[k]
        print(f"{k:<14}{s['last']:>7}{s16(s['last']):>8}  {name_of(k)}")

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
