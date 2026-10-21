# Thermia Gateway on an ESP32-S3: Flashing and Installation

> ## ⚠️ WARNING: WORK IN PROGRESS – USE AT YOUR OWN RISK
>
> This project is still being developed and tested. I have it working on my own heat pump, but **that is no guarantee it will work on yours.** Your hardware, software versions or installation may be different.
>
> **Tested on:**
>
> | Item | Version |
> |------|---------|
> | Heat pump | Danfoss (now Thermia) ATEC |
> | Heat pump software version | 2.2.0 |
> | DCM (Thermia Online gateway) version | 2.0.15 |
>
> If your versions are different, it may not work, or it may behave differently.
>
> You are working on your own heating system. If something goes wrong, you are responsible for the result. Keep your original DCM so you can always put things back the way they were (see "Going back to the original gateway" at the end of this guide).

This guide takes you from a brand-new Waveshare ESP32-S3-RS485-CAN board to one that is wired into the heat pump, running the gateway firmware, and showing up in Home Assistant. No previous ESP32 or electronics experience is assumed.

It is in three parts:

1. **Flash the firmware** on your desk, with the board plugged into your computer.
2. **Wire it into the heat pump** at terminal 121.
3. **Check it's working** in Home Assistant.

Do them in that order. The board should be flashed and tested **before** it goes anywhere near the heat pump.

---

## What you are actually doing (in plain English)

The ESP32 is a tiny computer. Out of the box it does nothing useful. "Flashing" means copying a program (the "firmware") onto it over a USB cable.

We don't write that program by hand. We use a free tool called **ESPHome**. You give ESPHome a settings file (`thermia-gateway.yaml`) plus a folder of code (`components/`), and ESPHome turns them into firmware and copies it onto the board.

Once it is wired in, the ESP32 takes the place of the Thermia Online gateway (the "DCM"). It talks to the heat pump over the RS485 bus at terminal 121, and passes the data on to Home Assistant over Wi-Fi.

After the first flash over USB, future updates can be sent over Wi-Fi ("OTA", over-the-air), so you only need the USB cable once.

---

## What you need

### For flashing

| Item | Notes |
|------|-------|
| Waveshare ESP32-S3-RS485-CAN | The board itself ([Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)) |
| USB-C cable | Must be a **data** cable. Some cheap cables only charge and will not work. If the computer doesn't react when you plug the board in, try another cable first. |
| A computer | Windows, Mac or Linux |
| Google Chrome or Microsoft Edge | Needed for flashing from the browser. Firefox and Safari can't talk to USB devices. |
| Your Wi-Fi name and password | **2.4 GHz** network. The ESP32 cannot join 5 GHz-only networks. |
| The project files | See Step 1 |
| Home Assistant (recommended) | Makes everything easier. Option B below works without it. |

### For wiring

| Item | Notes |
|------|-------|
| Twisted-pair cable (2 pairs) | e.g. CAT5/CAT6 or shielded LiYCY. One pair for RS485 A/B, one pair for 12 V/GND. |
| Twin-entry bootlace ferrules and a crimp tool | Recommended, because each terminal will hold two wires (the room stat's and the ESP's). |
| Small flat-blade screwdriver | For the terminal screws |
| Multimeter (optional but recommended) | To check the 12 V supply before connecting |
| DIN rail space (optional) | The Waveshare enclosure clips onto a DIN rail |

---

## ⚠️ Safety – read this first

- **Isolate the heat pump at the mains before opening any panels.** The heat pump contains mains voltage. Terminal 121 is low voltage, but other terminals nearby are not.
- If you are not competent working inside mains equipment, have a qualified electrician do Part 2.
- Check your warranty and service terms before modifying the installation.
- **Never connect the USB cable while the board is wired into the heat pump.** USB is for flashing on the desk only.

---

# Part 1 – Flash the firmware

## Step 1 – Get the project files laid out correctly

This is the step people most often get wrong, so take it slowly.

ESPHome needs to find the settings file and the code folder side by side, arranged **exactly** like this:

```
(your ESPHome folder)/
├── thermia-gateway.yaml
├── secrets.yaml                  ← you create this in Step 2
└── components/
    └── thermia_gateway/
        ├── __init__.py
        ├── alarm_bits.h
        ├── cm930_handshake.h
        ├── gateway_responder.h
        ├── gateway_shadow.h
        ├── modbus_rtu.h
        ├── reg_ranges.h
        ├── register_store.h
        ├── registers_json.h
        ├── registers_json_handler.h
        ├── thermia_gateway.h
        └── thermia_gateway.cpp
```

Check these points:

1. The folder names are exactly `components` and then `thermia_gateway` (lower case, underscore, no spaces).
2. The `thermia_gateway` folder contains **those 12 files and nothing else.**
3. **Do not copy any `test_*.cpp` file (or the `tests/` folder) into `components/thermia_gateway/`.** The test files are for checking the code on a PC. ESPHome tries to build every `.cpp` it finds in that folder, and the test files will break the build.
4. Leave out the `.md` documentation files, the `.csv` file and the two Python scripts (`thermia_collector.py`, `thermia_report.py`). None of them belong in the ESPHome folder.

> If you have older copies of the project, the names used to be `thermia-sniffer.yaml` and `components/thermia_sniffer/`. Don't mix old and new files. Use only the `thermia_gateway` versions.

Where "your ESPHome folder" is depends on which option you pick in Step 3.

## Step 2 – Create your secrets file

The settings file deliberately doesn't contain your Wi-Fi password. Instead it says `!secret wifi_password`, which means "look this up in `secrets.yaml`". This keeps passwords out of the main file.

Create a plain text file called `secrets.yaml` next to `thermia-gateway.yaml` containing:

```yaml
wifi_ssid: "YourWiFiName"
wifi_password: "YourWiFiPassword"
thermia_api_encryption_key: "PASTE-A-GENERATED-KEY-HERE"
```

**The encryption key** is a random code that lets Home Assistant talk to the board securely. It must be a 32-byte key written in base64 (it looks something like `k3Jd9...=` and is 44 characters long). To get one:

- Easiest: open the ESPHome documentation page for the **Native API** component (search "ESPHome native API encryption"). It has a box that generates a fresh random key each time you load the page. Copy it.
- Or, if you have Python installed, run:
  ```
  python -c "import secrets,base64;print(base64.b64encode(secrets.token_bytes(32)).decode())"
  ```

**Write the key down somewhere safe.** Home Assistant will ask for it in Step 6. The same key also protects Wi-Fi updates.

If you use the Home Assistant option below, ESPHome already has a shared `secrets.yaml` (there is a **SECRETS** button at the top right of the ESPHome dashboard). Just add these three lines to it rather than making a new file.

## Step 3 – Install ESPHome

Pick **one** of these. If you already run Home Assistant, choose Option A.

### Option A – ESPHome inside Home Assistant (recommended)

1. In Home Assistant go to **Settings → Add-ons → Add-on Store**.
2. Search for **ESPHome Device Builder**, open it and click **Install**.
3. Turn on **Show in sidebar**, then click **Start**.
4. Click **ESPHome Builder** in the sidebar. You'll see an empty dashboard. That's normal.

You now need to copy the files from Step 1 into Home Assistant's `/config/esphome/` folder. The simplest way for a beginner:

1. Install the **File editor** or **Studio Code Server** add-on (same Add-on Store).
2. Open it and navigate to the `esphome` folder (inside `config`).
3. Upload `thermia-gateway.yaml` there.
4. Create the folders `components`, then `thermia_gateway` inside it, and upload the 12 component files into `thermia_gateway`.
5. Add the three secrets to `secrets.yaml` (in the same `esphome` folder).

Go back to the ESPHome dashboard and refresh the page. A card called **danfoss-dhp-aq** should appear. If it doesn't, the YAML file is in the wrong folder.

### Option B – ESPHome on your computer (no Home Assistant needed)

1. Install **Python 3** from python.org. On Windows, tick **"Add Python to PATH"** during installation.
2. Open a terminal (Windows: search for "Command Prompt"; Mac: "Terminal").
3. Install ESPHome:
   ```
   pip install esphome
   ```
4. Check it worked:
   ```
   esphome version
   ```
5. Put the files from Step 1 into a folder of your choice, for example `Documents/thermia`.

**ESPHome version:** the settings file uses a newer style of Wi-Fi update security that needs **ESPHome 2026.9 or later**. If `esphome version` shows something older, run `pip install --upgrade esphome`. (In Home Assistant, update the add-on instead.)

## Step 4 – Check the settings before building

You shouldn't need to change anything. The file is already set up for this board and this heat pump. For reference, these are the important values, so you know what you're looking at:

| Setting | Value | What it means |
|---------|-------|---------------|
| Board | ESP32-S3, 16 MB flash | Matches the Waveshare board |
| UART TX pin | GPIO17 | ESP → RS485 chip (wired inside the board) |
| UART RX pin | GPIO18 | RS485 chip → ESP (wired inside the board) |
| RS485 transmit enable | GPIO21 | Only switched on when the ESP is acting as the gateway |
| Baud rate | 9600 | Bus speed |
| Parity / stop bits | Even / 1 (8E1) | Bus format |
| Modbus address | 15 (0x0F) | The address the original Thermia gateway used |
| Device name | `danfoss-dhp-aq` | What it's called in Home Assistant |

The GPIO pins are connections **inside** the Waveshare board. You don't wire anything to them yourself. Your only wires go to the screw terminals (Part 2).

**One setting worth understanding:** `auto_takeover: true`. With this on, a few seconds after it powers up the ESP listens for the original Thermia gateway. If it hears none, it starts acting as the gateway by itself. This is what you want once the original gateway is unplugged. If it ever hears the real gateway, it steps aside automatically, so the two can't fight each other.

## Step 5 – Flash the board over USB (first time only)

Plug the board into your computer with the USB-C cable. Nothing else should be connected to it.

### Option A (Home Assistant)

1. On the **danfoss-dhp-aq** card click the **⋮** menu, then **Validate**. This checks the file for mistakes without building anything. Fix any errors it reports (usually a missing secret or a file in the wrong place).
2. Click **⋮ → Install**.
3. Choose **Manual download**, then **Factory format**. ESPHome now builds the firmware.
   - **The first build is slow**, often 10–30 minutes on a Raspberry Pi. That's normal. Later builds are much faster.
4. When it finishes, a `.bin` file downloads to your computer.
5. In Chrome or Edge go to **web.esphome.io**.
6. Click **Connect**, choose the board from the list (it may show as "USB JTAG/serial debug unit" or a COM port), then click **Install** and select the `.bin` file you downloaded.
7. Wait for it to say the installation is complete.

(Why the detour? Flashing straight from the Home Assistant page only works if Home Assistant is opened over HTTPS. The download route always works.)

### Option B (computer)

In the terminal, go to your project folder and run:

```
cd Documents/thermia
esphome run thermia-gateway.yaml
```

ESPHome builds the firmware (slow the first time) and asks how to upload it. Choose the **USB / serial port** option, not "Over The Air".

### If the board isn't detected

1. Try a different USB-C cable (charge-only cables are the usual culprit).
2. Try a different USB port, ideally directly on the computer rather than through a hub.
3. Put the board into download mode by hand: **hold the BOOT button, tap the RESET button, then let go of BOOT.** Then try connecting again.
4. After flashing in this mode you may need to press RESET once more to start the new firmware.

## Step 6 – Check it on the desk and add it to Home Assistant

After flashing, the board restarts and should join your Wi-Fi within a minute.

**Look at the logs.** Option A: **⋮ → Logs** on the device card. Option B: `esphome logs thermia-gateway.yaml`. You should see:

- It connecting to Wi-Fi and getting an IP address. **Write the IP address down.**
- A start-up block headed **"Thermia gateway replacement"** listing the gateway address, takeover and auto-takeover settings.
- **No** lines saying `CRASH DETECTED` or `OTA rollback`.

On the desk there's no heat pump attached, so you won't see any bus data and most sensors will say "unknown". **That's expected.**

**Add it to Home Assistant:**

1. Go to **Settings → Devices & services**. A discovered **ESPHome** device called `danfoss-dhp-aq` should appear. Click **Configure**.
2. If it doesn't appear on its own, click **Add Integration → ESPHome** and type the board's IP address.
3. When asked, paste the **encryption key** from Step 2.

The device will appear with its sensors grouped into sections (Heating & Curve, Hot Water, Temperatures, and so on).

**Now unplug the USB cable.** From here on the board gets its power from the heat pump.

---

# Part 2 – Wire it into the heat pump

## How it connects

The ESP32 connects to **terminal block 121**, the room stat connection. It shares these terminals with the room stat ("doubles up"), taking both its 12 V power and its RS485 connection from the heat pump. No external power supply is needed, and the room stat stays connected and working as normal.

### The board's terminals

- **Power input** (screw terminal): accepts 7–36 V DC, so the heat pump's 12 V is fine.
- **RS485** (screw terminal): isolated, with its own on-board isolated supply.
- **120 Ω termination jumper:** next to the RS485 terminal. **It must NOT be fitted** (see below).
- **USB-C:** for flashing only. **Never plug it in while the board is powered from the heat pump.**

## Connection table

The ESP32 wires go into terminal 121 **alongside** the existing room stat wires. **Do not remove the room stat.**

| Heat pump terminal | Function | Room stat | ESP32-S3-RS485-CAN terminal |
|--------------------|----------|-----------|-----------------------------|
| 121.1 | +12 V supply | Stays connected | Power **+** (DC 7–36 V) |
| 121.2 | — | Stays connected (if used) | Not connected |
| 121.3 | RS485 A | Stays connected | RS485 **A+** |
| 121.4 | RS485 B | Stays connected | RS485 **B−** |
| 121.5 | GND | Stays connected | Power **−** (power-supply side) |

## Wiring diagram

```
                     Room stat (existing)
                        │ │ │ │ │
 Heat pump terminal 121 │ │ │ │ │      Waveshare ESP32-S3-RS485-CAN
 ┌──────────┐           │ │ │ │ │      ┌──────────────────────────┐
 │ 121.1 +12V├──────────┴─┼─┼─┼─┼──────┤ PWR +   (7–36 V DC)      │
 │ 121.2     ├────────────┘ │ │ │      │                          │
 │ 121.3  A  ├──────────────┴─┼─┼──────┤ RS485 A+                 │
 │ 121.4  B  ├────────────────┴─┼──────┤ RS485 B−                 │
 │ 121.5 GND ├──────────────────┴──────┤ PWR −   (power side)     │
 └──────────┘                          │ RS485 GND → NOT CONNECTED│
                                       │ 120Ω jumper → NOT FITTED │
                                       └──────────────────────────┘
```

## Three things to get right

### 1. The 120 Ω termination jumper must NOT be fitted

- The RS485 bus is already terminated inside the heat pump. Adding the ESP's resistor puts extra load on the bus, which can disrupt communication between the heat pump and the room stat.
- **Check the jumper before installation.** Some boards arrive with it fitted, or it may have been moved during testing. If it's there, pull it off and keep it somewhere safe.

### 2. Grounding

- **121.5 (GND) goes to the power-side negative terminal only.**
- **Do not connect any ground to the RS485 side of the board.** The RS485 port is electrically isolated from the rest of the board on purpose. Connecting its ground would bypass that isolation and can create ground loops (a common cause of unreliable communication).
- If you use shielded cable, connect the shield **at the heat pump end only**, or leave it unconnected. Never connect it at both ends.

### 3. Doubling up on terminal 121

Each terminal will hold two wires: the room stat's and the ESP's.

- Crimp the room stat wire and the ESP wire together into a **twin-entry ferrule** for each terminal, then clamp the ferrule.
- If you are not using ferrules, make sure both conductors are fully clamped. **Tug-test each wire separately.**
- Do not over-tighten small terminals.

## Installation steps

1. **Flash and test the board first** (Part 1). Confirm it joins your Wi-Fi and appears in Home Assistant, then unplug the USB cable.
2. **Check the termination jumper.** Confirm the 120 Ω jumper next to the RS485 terminal is **removed**.
3. **Isolate the heat pump** at the mains supply and confirm it is dead.
4. **Disconnect the Thermia Online gateway (DCM)**, if it's still fitted. The ESP32 replaces it, so the two shouldn't be on the bus at the same time. Keep it somewhere safe in case you want to go back.
5. **Photograph terminal 121** before touching it, so you have a record of the room stat wiring.
6. **Check the 12 V supply (optional but recommended).** Briefly restore power and measure between 121.1 and 121.5. You should see about 12 V DC. **Isolate the heat pump again** before continuing.
7. **Wire the ESP32 into terminal 121** alongside the room stat wires, following the connection table.
   - Use one twisted pair for A and B.
   - Use the other pair for +12 V and GND.
   - Use twin ferrules and tug-test every wire, including the room stat wires.
8. **Mount the ESP32** on a DIN rail or somewhere secure, away from mains cables and contactors, and where Wi-Fi reception is reasonable. A metal heat pump case can weaken the signal.
9. **Restore power** to the heat pump.
10. **Check the LEDs on the board:**
    - Red PWR LED: on.
    - RS485 LEDs: blue (receive) and green (transmit) flashing. This shows bus traffic.
11. **Check the room stat** still works: it shows a temperature, and the heat pump has no communication alarm.

---

# Part 3 – Check it's working

Give it a minute or two after power-up, then check these entities in Home Assistant (on the `danfoss-dhp-aq` device page):

| Entity | What you want to see |
|--------|----------------------|
| **Takeover Status** | "disarmed" for the first few seconds, then **ARMED** |
| **Takeover Arm** switch | On |
| **Gateway 0x0F Replying** | Off. This means "is the *original* gateway replying". Off is correct once the DCM is disconnected. |
| **Seconds Since HP Update** | A small number that keeps dropping back down. If it keeps climbing, the heat pump has stopped talking to the ESP. |
| **Mailbox Reads Served** | Steadily going up |
| **Mailbox Reads Dropped (bus busy)** | 0 or very close to it |
| Temperature sensors | Real values appearing within about 30 seconds |
| Settings (heating curve, hot water etc.) | Filled in shortly after the ESP arms |

**Optional extra check:** in a browser, go to `http://<the ESP's IP address>/registers.json`. You should see a page of numbers starting with `{"uptime_s":`. This is a read-only list of everything the ESP has received from the heat pump. Nothing on that page can change any settings. (It's used by the optional register logger: see `README-register-logging.md`.)

Leave it running for a few days and glance at **Seconds Since HP Update** and **Mailbox Reads Dropped** now and then.

---

## Updating the firmware later (over Wi-Fi)

You don't need to take the board out or open the heat pump to update it.

- Option A: **⋮ → Install → Wirelessly**.
- Option B: `esphome run thermia-gateway.yaml`, then choose the **Over The Air** option.

The ESP restarts after an update. That's fine: it should go back to **ARMED** within a few seconds.

After every update, check **Takeover Status** shows ARMED and **Seconds Since HP Update** stays low. If it doesn't recover within a couple of minutes, switch the heat pump off at its isolator, wait a few seconds, and switch it back on.

---

## Troubleshooting

### Building and flashing

| Problem | What to try |
|---------|-------------|
| Validate / build fails with "secret not found" | A name in `secrets.yaml` is missing or misspelt. You need exactly `wifi_ssid`, `wifi_password` and `thermia_api_encryption_key`. |
| Build fails mentioning `thermia_gateway` or "component not found" | The `components/thermia_gateway/` folder is in the wrong place or misnamed. It must sit right next to `thermia-gateway.yaml`. |
| Build fails with "multiple definition of `main`" | A test `.cpp` file has ended up in the component folder. Remove it. Only the 12 listed files belong there. |
| Build fails with "No such file" for a `.h` file | One of the 12 component files is missing. Check the list in Step 1. |
| Build fails inside `registers_json_handler.h` | A newer ESPHome may have changed something this file relies on. Note the error and report it to whoever maintains the project. |
| Build fails on the `ota:` / `encryption:` section | Your ESPHome is older than 2026.9. Update it. |
| Board never joins Wi-Fi | Check the Wi-Fi name and password for typos (they're case-sensitive), and that it's a 2.4 GHz network. Look at the logs over USB for the reason. |
| Home Assistant says the encryption key is wrong | Paste the exact key from `secrets.yaml`, including any `=` at the end. |
| Wi-Fi update fails with an authentication error | The board is running much older firmware. Flash once over USB instead (Step 5). |

### Once wired in

| Problem | What to try |
|---------|-------------|
| No red power LED | Check 121.1 → PWR+ and 121.5 → PWR−. Measure 12 V at the board's power terminals. |
| Power OK, but the blue RS485 LED never flashes | Check the A/B wiring. Try swapping A and B: different manufacturers label them differently. |
| Blue LED flashes but no data / CRC errors in the log | Confirm the 120 Ω jumper is **not** fitted. Check the A/B pair is twisted together and routed away from mains cables. |
| Room stat stops working or shows an error | Check the room stat wires are still clamped in terminal 121. Confirm the 120 Ω jumper is **not** fitted. Disconnect the ESP to see whether it's the cause. |
| Intermittent communication | Route the cable away from mains and compressor wiring. Check the shield is connected at one end only. |
| Heat pump shows a communication alarm | Make sure the DCM is disconnected. Check the jumper. Check **Takeover Status** in Home Assistant. |
| Takeover Status never reaches ARMED | Make sure the DCM is disconnected, then power-cycle the heat pump. |
| Takeover Status says the real gateway was heard | The original Thermia Online gateway is still connected. Unplug it and power-cycle the heat pump. |
| Board resets or behaves oddly | Make sure USB-C is **not** plugged in while it's powered from the heat pump. |

---

## Going back to the original gateway

Nothing here is permanent.

1. Isolate the heat pump.
2. Remove the ESP32 wires from terminal 121, leaving the room stat wires in place and secure.
3. Reconnect the DCM as originally installed.
4. Restore power and check the room stat works.

If you ever reconnect the DCM with the ESP still wired in, restart the heat pump. The ESP notices the real gateway within a fraction of a second and stops acting as the gateway, so the two won't clash.
