# Build, flash, and operate

**Last updated:** 21 October 2026

## Layout

```
thermia-gateway.yaml                      ESPHome device config
components/thermia_gateway/               the custom component (12 files, see below)
tests/                                    host-only C++ tests (NEVER copy into the component)
Documentation/*.md                        docs (start with HANDOFF.md)
```

**Critical:** the `tests/` folder must stay OUTSIDE `components/thermia_gateway/`. ESPHome
compiles every `.cpp` in a component folder; the test files have their own `main()` and read
capture logs from disk, so if they land in the component folder the firmware build breaks.
The component folder should contain exactly these 12 files (no test `.cpp` files):
`__init__.py, thermia_gateway.h, thermia_gateway.cpp, gateway_responder.h, gateway_shadow.h,
cm930_handshake.h, modbus_rtu.h, register_store.h, registers_json.h, registers_json_handler.h,
reg_ranges.h, alarm_bits.h`

## Build / flash

Standard ESPHome. Point your ESPHome config dir at `thermia-gateway.yaml` with the
`components/` folder alongside it (the YAML references the external component by path).

```
esphome run thermia-gateway.yaml
```

Requires: secrets for WiFi / API / OTA (the YAML uses !secret). An ESP32-S3. RS485
transceiver (SP3485) on GPIO17 (TX), GPIO18 (RX), GPIO21 (driver-enable, DE+/RE tied).

## Running the host tests (optional, on a PC)

```
cd tests
g++ -std=c++17 -Wall -Wextra -I../components/thermia_gateway test_step1.cpp -o t && ./t
```
The replay tests (step1/2/4a/4b) read capture `.log` files — edit the path near the top of
each file to point at wherever you keep the captures. test_step5 is self-contained.

## Operating

### Config knobs (in thermia-gateway.yaml, under `thermia_gateway:`)

- `takeover_capable: true` — allows the ESP to transmit/arm at all. Requires
  `flow_control_pin`.
- `flow_control_pin: GPIO21` — RS485 driver-enable.
- `auto_takeover: true` — the fully-local permanent setup: the ESP self-arms after
  `auto_listen_ms` if no real gateway is heard, then auto-seeds values via a resync once the
  heat pump addresses it ("takeover on at load"). Set `false` to require the manual toggle.
- `auto_listen_ms: 8s` — how long to listen for a real gateway before self-arming. Short is
  fine (a running gateway is heard within ~4 s); set back to `20s` only if you run with the
  real gateway still on the bus.

### HA entities you'll use

- **Takeover Arm** (switch) — reflects the real armed state; turn OFF to disarm until the
  next reboot (manual override) without unplugging anything; turn ON to re-arm.
- **Takeover Status** (text) — ARMED / disarmed / why.
- **Takeover TX Frames** (number) — frames the ESP has transmitted as the gateway.
- **Request Full Resync** (button) — armed only; makes the heat pump re-push its whole
  settings table (seeds the store).
- **Hot Water Start Temp (write)** (number) — shows the real current 1053 once seen; setting
  it (while armed) writes to the heat pump. The read-back sensor updates once the heat pump
  re-reads the block (a few seconds). Writes raise the flag twice, matching the real gateway.
- **Seconds Since HP Update** (sensor) — resets whenever the heat pump writes any register. If
  it climbs without bound, the heat pump has stopped pushing (investigate).
- **Mailbox Reads Served** / **Mailbox Reads Dropped (bus busy)** (sensors) — mailbox serving
  health. Served should climb steadily; Dropped should stay at or near 0.
- **Handshake Check**, **Gateway Shadow Check**, **Active Alarm Bits**, plus all the decoded
  sensors grouped into sub-devices.

## Going fully local (the intended end state)

1. The shipped config is already fully local: `auto_takeover: true`, `auto_listen_ms: 8s`,
   real gateway unplugged. The ESP self-arms a few seconds after boot and seeds values.
2. Soak test (several days) watching the diagnostics above and for anything unusual.
3. To confirm a clean takeover, power-cycle the ESP (or both ESP and heat pump); it should
   self-arm within ~8 s and populate values via the auto-resync.
4. Keep the real gateway on the shelf as recovery. Set `auto_takeover: false` if you ever want
   to go back to manual arming.

## Recovery (always available)

Plug the real gateway back in and reboot the heat pump. The ESP auto-disarms the instant it
hears the real gateway, so there is no lasting two-gateway conflict. A reboot also always
returns the ESP to its default (disarmed, then auto per config).

## First-boot checklist after any flash

- Log shows the config banner with the takeover/auto-takeover lines.
- No `CRASH DETECTED` / `OTA rollback`.
- `Takeover Status` sensible for your config: with `auto_takeover: true` it is disarmed for
  the first few seconds, then ARMED once the listen window passes with no real gateway heard.
- `Seconds Since HP Update` low and resetting; `Mailbox Reads Dropped` at/near 0.
- `Gateway Shadow Mismatches` and handshake mismatches at 0.
