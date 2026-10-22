#!/usr/bin/env python3
"""thermia_collector.py  v2026.10.20.3  - log ESP register values over time, SD-card friendly.

Runs on the Home Assistant machine, started every 5 minutes by a shell_command (see
README-register-logging.md). Python standard library only.

Each run fetches http://<esp>/registers.json (raw unsigned 16-bit registers + live I/O) and
records only what CHANGED since the previous run.

Storage (designed for an SD card):
  * One file per day: <log-dir>/thermia-YYYY-MM-DD.jsonl.gz
    - first line of each day: a full snapshot of every value;
    - then one line per run in which something changed, holding only the changed values.
    The file is a series of appended gzip members, which gzip/zcat/Python read as one stream.
  * Changes are buffered in RAM (<state-dir>, default /dev/shm = tmpfs) and appended to the
    SD card at most once per --flush-minutes (default 60) as ONE gzip member. A Home
    Assistant restart or power cut loses at most that much buffered data.
  * Files older than --retention-days (default 30) are deleted when a new day starts.

Line format (one JSON object per line):
  {"t": 1729400000, "type": "snapshot", "uptime_s": 123, "v": {"1000": 22, "dev02.w.10": 5, ...}}
  {"t": 1729400300, "type": "delta", "v": {"2120": 65485}}
  {"t": 1729400600, "type": "delta", "restart": true, "v": {...}}   <- ESP uptime went down

A key missing from a fetch (e.g. just after an ESP reboot, or a live value gone stale) is NOT
recorded as a change; it keeps its last value until it is seen again.

Exit code is always 0 (a failed fetch is reported on stderr only, so Home Assistant does not
raise an error every 5 minutes while the ESP is offline).
"""

import argparse
import datetime as dt
import gzip
import json
import os
import sys
import time
import urllib.request

STATE_FILE = "state.json"      # last values + last uptime (RAM)
PENDING_FILE = "pending.jsonl"  # buffered lines not yet on the SD card (RAM)


def log(msg):
    print(f"thermia_collector: {msg}", file=sys.stderr)


def day_of(ts):
    return dt.datetime.fromtimestamp(ts).strftime("%Y-%m-%d")


def day_path(log_dir, day):
    return os.path.join(log_dir, f"thermia-{day}.jsonl.gz")


def fetch(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        data = json.loads(r.read().decode("utf-8"))
    values = {str(k): int(v) for k, v in data.get("reg", {}).items()}
    values.update({str(k): int(v) for k, v in data.get("live", {}).items()})
    return values, int(data.get("uptime_s", 0))


def read_day_file(path):
    """Rebuild the last known values from a day file (snapshot + deltas). Tolerates a damaged
    tail (e.g. a power cut mid-write): everything before the damage is used."""
    values, uptime, ok = {}, None, True
    try:
        with gzip.open(path, "rt", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                values.update(rec.get("v", {}))
                if "uptime_s" in rec:
                    uptime = rec["uptime_s"]
    except (OSError, EOFError, ValueError) as e:
        log(f"{path}: damaged after the last good record ({e}); continuing from what was read")
        ok = False
    return values, uptime, ok


def append_lines(path, lines):
    """Append lines to a day file as ONE gzip member (one SD write)."""
    if not lines:
        return
    with gzip.open(path, "at", encoding="utf-8", compresslevel=9) as f:
        f.write("".join(lines))


def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, separators=(",", ":"))
    os.replace(tmp, path)


def flush_pending(state_dir, log_dir):
    """Move buffered lines to their day files (grouped by day: one write per day file)."""
    pend = os.path.join(state_dir, PENDING_FILE)
    try:
        with open(pend, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        return 0
    by_day = {}
    for line in lines:
        try:
            by_day.setdefault(day_of(json.loads(line)["t"]), []).append(line)
        except (ValueError, KeyError):
            continue
    for day, day_lines in sorted(by_day.items()):
        append_lines(day_path(log_dir, day), day_lines)
    os.remove(pend)
    return len(lines)


def prune(log_dir, keep_days, today):
    cutoff = (dt.datetime.strptime(today, "%Y-%m-%d") - dt.timedelta(days=keep_days)).strftime("%Y-%m-%d")
    for name in os.listdir(log_dir):
        if name.startswith("thermia-") and name.endswith(".jsonl.gz") and name[8:18] < cutoff:
            os.remove(os.path.join(log_dir, name))
            log(f"retention: deleted {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True, help="e.g. http://192.168.1.50/registers.json")
    ap.add_argument("--log-dir", default="/config/thermia_log")
    ap.add_argument("--state-dir", default="/dev/shm/thermia_collector",
                    help="RAM directory for the change buffer (tmpfs)")
    ap.add_argument("--flush-minutes", type=int, default=60, help="0 = write to the SD card every run")
    ap.add_argument("--retention-days", type=int, default=30)
    ap.add_argument("--timeout", type=float, default=20)
    ap.add_argument("--flush-now", action="store_true", help="only flush the RAM buffer (e.g. at HA shutdown)")
    ap.add_argument("--now", type=float, help=argparse.SUPPRESS)  # testing only
    a = ap.parse_args()

    os.makedirs(a.log_dir, exist_ok=True)
    try:
        os.makedirs(a.state_dir, exist_ok=True)
    except OSError as e:
        # No RAM directory available: keep state next to the logs and write every run instead.
        log(f"state dir {a.state_dir} unavailable ({e}); writing to the log dir every run instead")
        a.state_dir = os.path.join(a.log_dir, ".state")
        os.makedirs(a.state_dir, exist_ok=True)
        a.flush_minutes = 0
    state_path = os.path.join(a.state_dir, STATE_FILE)

    if a.flush_now:
        n = flush_pending(a.state_dir, a.log_dir)
        log(f"flushed {n} buffered line(s)")
        return

    now = a.now if a.now is not None else time.time()
    ts = int(now)
    today = day_of(ts)
    today_file = day_path(a.log_dir, today)

    try:
        current, uptime = fetch(a.url, a.timeout)
    except Exception as e:  # noqa: BLE001 - any failure: skip this run quietly
        log(f"fetch failed: {e}")
        return
    if not current:
        log("ESP returned no values yet (just booted?); nothing recorded")
        return

    state = load_json(state_path, None)

    # New day (or first ever run): flush what belongs to earlier days, then a full snapshot.
    if not os.path.exists(today_file):
        flush_pending(a.state_dir, a.log_dir)
        last = state["values"] if state else {}
        merged = dict(last)
        merged.update(current)  # carry values not in this fetch, so the snapshot is complete
        append_lines(today_file, [json.dumps({"t": ts, "type": "snapshot", "uptime_s": uptime, "v": merged},
                                             separators=(",", ":")) + "\n"])
        prune(a.log_dir, a.retention_days, today)
        save_json(state_path, {"values": merged, "uptime_s": uptime, "last_flush": ts})
        return

    # RAM state lost (HA restart): rebuild from today's file (snapshot + flushed deltas).
    if state is None:
        values, up, ok = read_day_file(today_file)
        if not ok:
            os.replace(today_file, today_file + ".damaged")
            append_lines(today_file, [json.dumps({"t": ts, "type": "snapshot", "uptime_s": uptime,
                                                  "v": {**values, **current}}, separators=(",", ":")) + "\n"])
            save_json(state_path, {"values": {**values, **current}, "uptime_s": uptime, "last_flush": ts})
            return
        state = {"values": values, "uptime_s": up, "last_flush": ts}

    last = state["values"]
    changed = {k: v for k, v in current.items() if last.get(k) != v}
    restarted = state.get("uptime_s") is not None and uptime < state["uptime_s"]

    if changed or restarted:
        rec = {"t": ts, "type": "delta", "v": changed}
        if restarted:
            rec["restart"] = True
        if a.flush_minutes <= 0:
            append_lines(today_file, [json.dumps(rec, separators=(",", ":")) + "\n"])
        else:
            with open(os.path.join(a.state_dir, PENDING_FILE), "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        last.update(changed)

    if a.flush_minutes > 0 and ts - state.get("last_flush", ts) >= a.flush_minutes * 60:
        flush_pending(a.state_dir, a.log_dir)
        state["last_flush"] = ts

    state["values"] = last
    state["uptime_s"] = uptime
    save_json(state_path, state)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 - never fail the HA shell_command
        log(f"unexpected error: {e}")
    sys.exit(0)
