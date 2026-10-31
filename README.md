# Lab Power Supply

A small bench power supply you control from your phone or browser. You plug in a USB-C
laptop charger and get an adjustable output from **4.5 V to 21 V** (up to 48 V with an EPR
charger), with current, voltage and power cutoffs, live charts and a measurement log.

It's built from two cheap boards: a **Wemos D1 mini** (ESP8266, Wi-Fi) driving a
**[CentyLab RotoPD Pro](https://hackaday.io/project/204943-rotopd-pro-usb-c-240w-breakout-i2c)**
(USB-PD sink + power monitor).

## Features

- **Any voltage** the charger allows: fine steps with PPS/AVS chargers, or its fixed profiles
  (5 / 9 / 12 / 15 / 20 V…)
- **Live readout** of voltage, current, power and temperature, and the voltage drop under load
- **Charts**: the last 2 minutes live, or the last 10 min, 1 h or the whole log
- **Protection**: current, voltage and power cutoffs that act in hardware within milliseconds
- **Soft start**: the voltage ramps up instead of jumping
- **Charging helpers**: switch off after a set time, after a set charge (mAh), or when the
  current drops (end of charge)
- **Counters**: on-time, mAh and Wh
- **Measurement log**, downloadable as CSV
- **USB-PD tester**: lists what a charger offers, guesses the cable rating and measures the
  cable resistance
- Wireless firmware updates, an optional login, and a serial console

## What you need

- Wemos D1 mini (ESP8266)
- CentyLab RotoPD Pro
- A USB-C PD charger (a PPS charger gives the most flexibility)
- 5 wires

## Wiring

| D1 mini     | RotoPD Pro |
|-------------|------------|
| D2 (GPIO4)  | SDA        |
| D1 (GPIO5)  | SCL        |
| GND         | GND        |
| 5V          | 5V Out     |
| 3V3         | 3.3V In    |

The charger powers the RotoPD, and the RotoPD's **5V Out** powers the D1 mini. The D1 mini
sends 3.3 V back for the I²C signals.

> [!WARNING]
> - Watch the crossover: **SCL → D1**, **SDA → D2**.
> - Never connect the RotoPD's **power output** (up to 48 V) to the D1 mini.
> - Unplug the 5V wire before plugging the D1 mini into a computer over USB, so the two 5 V
>   sources aren't tied together. After the first flash, Wi-Fi updates avoid this.

## Getting started

### 1. Install the tools

The firmware is built with [PlatformIO](https://platformio.org/). Install it in a local
Python environment:

```sh
git clone git@github.com:CorentinGrard/Lab-Power-Supply.git
cd Lab-Power-Supply
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
source .venv/bin/activate
```

### 2. Add your Wi-Fi details

```sh
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` and fill in your Wi-Fi name and password. You can also set a password
for wireless updates and one for the web page. This file is never committed.

### 3. Flash the D1 mini

The first time, flash over USB (5V wire unplugged):

```sh
pio run -e d1_mini -t upload
```

After that, the board can be updated over Wi-Fi, even while it's wired up:

```sh
pio run -e d1_mini_ota -t upload
```

### 4. Open the page

Wire the boards, plug in the charger and go to **http://labpsu.local**.

If the board can't join your network within 15 s, it starts its own Wi-Fi hotspot,
**LabPSU** (password `labpsu123`). Connect to it and open http://192.168.4.1.

## Using it

1. Pick a voltage, either in **Adjustable** (PPS/AVS chargers) or under **Fixed voltages**.
2. In **Protection**, set the current cutoff to what your circuit should draw at most.
3. Press the **OUTPUT OFF** button to switch the output on.

If a cutoff trips, the output switches off and a banner explains why. Press **Reset** in the
banner, then switch the output on again.

Settings are saved automatically and come back after a restart. For safety, the output is
always **off** when the board starts.

### Charging a battery

Under **Counters** you can make the output switch off:

- after a set time,
- after a set charge in mAh, or
- once the current stays below a value for some seconds (end of charge).

### Testing a charger or cable

Plug any USB-C charger in and open **Charger & cable**. You'll see every profile it offers
with its power, whether it supports PPS/AVS/EPR, what the cable is probably rated for, and
(after drawing over 0.3 A) the measured cable resistance. The same report is printed by the
`pd` serial command. [How it works](#charger--cable-report-details).

## Troubleshooting

**The board keeps rebooting at low voltages.** The D1 mini runs from the RotoPD's 5V Out,
which follows the output voltage down. Below about 4.2 V it browns out, so the firmware
won't go below 4.5 V. If you power the D1 mini from USB instead, set `MIN_SUPPLY_MV` to 0 in
`src/main.cpp`.

**The web page doesn't show any charger profiles.** The RotoPD can still be negotiating when
the D1 mini starts. The firmware keeps retrying every 2 s, and the page updates by itself.

**`labpsu.local` doesn't open.** Some networks don't support `.local` names. Use the IP
address printed on the serial console at boot, or run the `wifi` command.

**USB flashing fails.** On the clone used here:

- The USB chip is a CH9102 and appears as `/dev/ttyACM0`, not `ttyUSB`.
- Many USB-C cables are charge-only. If no `ttyACM` device appears, try another cable.
- Auto-reset doesn't work. Connect **D3 to G**, tap **RST**, run the upload, then remove the
  wire and tap RST again.
- Fallback: a CH341A in UART mode (TXD → RX, RXD → TX, GND → G) with the same D3 trick. If
  the port echoes what you send, TX and RX are shorted.

## Safety

- The output is **off** at boot and during wireless updates.
- **Cutoffs work in hardware.** The INA238's alert pin drives a MOSFET that disables the
  output switch within a few ms of a reading over the limit, without waiting for the
  ESP8266. The firmware also checks the averaged readings every 20 ms as a backup. The
  default current cutoff is 5.1 A.
- The voltage cutoff measures the charger side. Voltage requests above it are refused.
- PPS chargers limit the current themselves (1 A minimum). AVS and fixed profiles don't, so
  set the current cutoff.
- Requests outside the charger's advertised range are refused.
- If no `WEB_PASSWORD` is set, anyone on your network can control the supply.

---

## Reference

### Build commands

```sh
pio run                              # build
pio run -e d1_mini -t upload         # flash over USB
pio run -e d1_mini_ota -t upload     # flash over Wi-Fi
pio device monitor                   # serial console, 115200 baud
```

If you set `OTA_PASSWORD`, add `upload_flags = --auth=<password>` to the `d1_mini_ota`
section of `platformio.ini`.

### Serial commands

| Command          | Effect                                              |
|------------------|-----------------------------------------------------|
| `help`           | list commands                                       |
| `wifi`           | network status and web address                      |
| `scan`           | scan the I²C bus                                    |
| `profile`        | list the charger's profiles                         |
| `stat`           | measurements, settings, counters, last event        |
| `on` / `off`     | output switch                                       |
| `set <mV> [mA]`  | any voltage: picks PPS, fixed or AVS                |
| `fix <idx> [mA]` | a fixed profile                                     |
| `pps <mV> [mA]`  | a PPS voltage (PPS charger, up to 21 V)             |
| `avs <mV> [mA]`  | an AVS voltage (EPR charger + 5 A cable, up to 48 V) |
| `ocp <mA>`       | current cutoff                                      |
| `ovp <mV>`       | voltage cutoff (0 = off)                            |
| `opp <mW>`       | power cutoff (0 = off)                              |
| `clear`          | reset a tripped cutoff                              |
| `ramp <ms>`      | soft start duration (0 = off)                       |
| `timer <s>`      | switch off after this much on-time (0 = off)        |
| `mah <mAh>`      | switch off after this much charge (0 = off)         |
| `endcharge <mA> [s]` | switch off below `<mA>` for `[s]` seconds (0 = off) |
| `pd`             | charger and cable report                            |
| `zero`           | reset the counters                                  |
| `log <s>`        | log interval (clears the log)                       |

Leaving out `[mA]` uses the charger's maximum.

### JSON API

Every request needs the login when `WEB_PASSWORD` is set (HTTP basic auth, user `admin`).

| Request | Effect |
|---------|--------|
| `GET /api/status` | measurements, setpoint, settings, counters |
| `GET /api/pdos` | charger profiles |
| `GET /api/log.csv[?since=<uptime_s>]` | the log as CSV (only newer entries with `since`) |
| `POST /api/set?mode=auto&mv=&ma=` | any voltage: picks PPS, then fixed, then AVS (`ma=0`: charger max) |
| `POST /api/set?mode=pps\|avs&mv=&ma=` / `?mode=fixed&idx=&ma=` | a specific profile |
| `POST /api/output?on=0\|1` | output switch |
| `POST /api/protect?ocp_ma=&ovp_mv=&opp_mw=&ramp_ms=` | cutoffs and soft start (any subset; 0 = off, except `ocp_ma`) |
| `POST /api/clear` | reset a tripped cutoff |
| `POST /api/timer?off_s=&off_mah=&end_ma=&end_s=` | output timer, charge limit, end of charge (0 = off) |
| `POST /api/counters/reset` | reset on-time / mAh / Wh |
| `POST /api/log?interval_s=` | log interval (clears the log) |

Example:

```sh
curl -X POST 'http://labpsu.local/api/set?mode=auto&mv=12000'
curl -X POST 'http://labpsu.local/api/output?on=1'
curl http://labpsu.local/api/status
```

### Charger & cable report details

- **Profiles** come from the AP33772S, which stores up to 7 standard and 6 EPR profiles.
  Currents are in 250 mA steps, as the chip reports them. The chip can't send arbitrary PD
  messages, so manufacturer IDs and other extended details aren't available.
- **Cable rating** is a guess: a charger only offers 5 A once it has read a 5 A e-marker in
  the cable, and only offers EPR profiles over an EPR cable. With 3 A profiles only, the
  limit could be the charger or the cable. The AP33772S can't read the e-marker itself.
- **Cable resistance** comes from the voltage drop under load at the same setpoint (the
  charger regulates at its connector, and the INA238 measures at ours). It needs more than
  0.3 A, includes the connectors, and reads low with chargers that compensate for cable drop.

### Measurement log

Readings are averaged over the log interval and the last 3000 entries are kept in RAM, so
the log is lost on reboot. Times are seconds since boot, plus Unix time once the board has
reached an NTP server.

### Hardware notes

- I²C addresses: `0x40` INA238, `0x52` AP33772S (`0x70` also answers, harmless).
- The header's `INT` pin is the AP33772S interrupt, not the INA238 alert.
- 5V Out is rated 0.4 A, which is plenty for the D1 mini.

### Tested with

A charger offering 5 / 9 / 12 / 15 V at 3 A, 20 V at 5 A and PPS 3.3–21 V at 3 A.

## Credits

- [AP33772S Arduino library](https://github.com/CentyLab/AP33772S-CentyLab) by CentyLab
- [INA238 library](https://github.com/RobTillaart/INA238) by Rob Tillaart
- [RotoPD Pro](https://hackaday.io/project/204943-rotopd-pro-usb-c-240w-breakout-i2c) by CentyLab
