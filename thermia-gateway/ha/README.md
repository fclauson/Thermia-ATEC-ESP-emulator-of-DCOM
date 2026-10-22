# Register logging — which ODU and unknown registers actually change

**Added:** 20 October 2026.

Logs every register the ESP holds, plus the live I/O values, every 5 minutes. The log is kept
on the Home Assistant machine. A report on your PC then shows which values change and which
stay fixed.

| Part | Where it runs | File |
|---|---|---|
| JSON endpoint | ESP (built into the component) | `registers_json.h`, `registers_json_handler.h` |
| Collector | Home Assistant machine, every 5 min | `thermia_collector.py` (this folder) |
| Report | Your PC | `thermia_report.py` (this folder) |

All Python is standard library only, so there is nothing to install.

## 1. ESP: `GET /registers.json`

After flashing, open `http://<esp-ip>/registers.json` in a browser. You should see:

```json
{"uptime_s":3600,"armed":true,
 "reg":{"1000":22,"1001":20,...},
 "live":{"dev02.w.10":5,"dev02.r.10":7,...}}
```

- **`reg`:** every register seen since the ESP booted, raw unsigned 16-bit. A negative
  register such as 1077 = −20 appears as 65516.
- **`live`:** the FC23 live I/O values, keyed `dev<device hex>.<w|r>.<address>`. `w` means the
  heat pump wrote the value to that device; `r` means the device replied with it. Only values
  updated in the last 60 s are included, so a device that stops talking drops out.
- **Refresh:** the JSON is rebuilt in the main loop every 30 s. The web server only copies that
  text and never reads the live data directly, so a request can't disturb the bus handling.
- **Read-only:** the endpoint can't change anything.

The component auto-loads ESPHome's `web_server_base`, so port 80 is open on the ESP. Only
this one URL is served; the full ESPHome web UI is not enabled.

If the ESPHome build fails in `registers_json_handler.h`, newer ESPHome versions may have
changed the request API (`url()`). Send me the error and I'll adjust it.

## 2. Home Assistant: the collector

1. Copy `thermia_collector.py` (from this folder) to `/config/thermia_collector/thermia_collector.py`. The
   Samba share or the File editor add-on both work.
2. Add this to `configuration.yaml`, replacing the IP address with your ESP's:

```yaml
shell_command:
  thermia_collect: "python3 /config/thermia_collector/thermia_collector.py --url http://192.168.1.50/registers.json"
  thermia_collect_flush: "python3 /config/thermia_collector/thermia_collector.py --url unused --flush-now"

automation thermia:
  - id: thermia_register_logger
    alias: Thermia register logger
    mode: single
    triggers:
      - trigger: time_pattern
        minutes: "/5"
    actions:
      - action: shell_command.thermia_collect
  - id: thermia_register_logger_flush
    alias: Thermia register logger flush at shutdown
    triggers:
      - trigger: homeassistant
        event: shutdown
    actions:
      - action: shell_command.thermia_collect_flush

# Keep the 5-minute automation out of the activity log and the database (SD card writes).
# If you already have recorder: or logbook: sections, add these lines to them instead.
recorder:
  exclude:
    entities:
      - automation.thermia_register_logger
      - automation.thermia_register_logger_flush
logbook:
  exclude:
    entities:
      - automation.thermia_register_logger
      - automation.thermia_register_logger_flush
```

`automation thermia:` (with the label) sits alongside your existing
`automation: !include automations.yaml` without clashing.

3. Restart Home Assistant. After 5 minutes, `/config/thermia_log/` should hold
   `thermia-YYYY-MM-DD.jsonl.gz`.

### What it writes, and how often

- **Daily snapshot:** each day's file starts with a full snapshot of every value. That's about
  3 KB compressed.
- **Changes only after that:** a 5-minute run where nothing changed writes nothing at all.
- **Buffered in RAM:** changes are kept in `/dev/shm` and written to the SD card **once an
  hour**, as one compressed chunk. That's about 24 small writes a day instead of 288. A Home
  Assistant restart or power cut can lose up to the last hour; the shutdown automation flushes
  first on a normal restart. Use `--flush-minutes 0` to write every run instead. If `/dev/shm`
  isn't available, the collector does this automatically.
- **Retention:** files older than 30 days are deleted when a new day starts
  (`--retention-days`). Expect roughly 50–100 KB a day, so a few MB in total.
- **Gaps are not changes:** a value missing from a fetch (just after an ESP reboot, or a stale
  live value) keeps its last value. An ESP reboot is recorded as a `restart` marker.
- **No error spam:** a failed fetch (ESP offline) is skipped quietly. The script always exits
  successfully, so Home Assistant doesn't raise an error every 5 minutes.

File format: one JSON object per line, with a full `"snapshot"` first and then `"delta"` lines
holding only the changed values. The file is a series of appended gzip chunks; `zcat`,
`gzip -dc` and Python all read it as one stream.

## 3. PC: the report

Copy the folder `/config/thermia_log` to your PC, then:

```text
python thermia_report.py thermia_log/ --names thermia-gateway.yaml
python thermia_report.py thermia_log/ --names thermia-gateway.yaml --only-unmapped --csv report.csv
python thermia_report.py thermia_log/ --since 2026-11-01
```

- **CHANGED:** for each changing key, the number of changes and distinct values, min/max as raw
  and as signed int16, the last value and the time of the last change. Sorted most-changing
  first.
- **FIXED:** keys that never changed over the period, with their value.
- **`--names`:** labels registers from the YAML, so unmapped ones (blank name) stand out.
  `--only-unmapped` hides the mapped ones.
- **`--csv`:** writes the full table for a spreadsheet.

**Tip for the ODU question:** run the report over a period that includes heating runs and
(once it's cold) defrosts. A register that stays fixed through compressor starts and defrosts
is probably a setting or constant, not a measurement.
