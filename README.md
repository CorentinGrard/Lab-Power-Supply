# Lab power supply

USB-C PD bench supply: a Wemos **D1 mini (ESP8266)** controls a **CentyLab RotoPD Pro**
(AP33772S PD 3.1 sink + INA238 power monitor) over I²C. Control it from a web page on
Wi-Fi or from the serial console.

## Wiring

| D1 mini     | RotoPD Pro |
|-------------|------------|
| D2 (GPIO4)  | SDA        |
| D1 (GPIO5)  | SCL        |
| GND         | GND        |
| 5V          | 5V Out     |
| 3V3         | 3.3V In    |

The RotoPD is powered by the USB-C charger. Its **5V Out** powers the D1 mini, whose
regulator then feeds 3.3 V back to the RotoPD's **3.3V In** (I²C logic level).

- Note the crossover in the names: **SCL → D1**, **SDA → D2**. 3.3 V logic only.
- Never connect the RotoPD's power output (up to 48 V) to the D1 mini.
- Before plugging the D1 mini into USB (e.g. for flashing), remove the 5V wire so the
  two 5 V sources aren't tied together. OTA avoids this.

I²C addresses: `0x40` INA238, `0x52` AP33772S (`0x70` also answers, harmless).

The firmware reads the charger's profiles at boot and keeps retrying every 2 s until it
gets them (the RotoPD may still be negotiating when the D1 mini starts). It also notices
when a different charger is plugged in: the output switches off and the profiles reload.
The web page picks up new profiles by itself.

Because 5V Out follows the charger voltage down, the D1 mini browns out and reboots below
about 4.2 V. PPS/AVS requests are therefore kept at **4.5 V or more**
(`MIN_SUPPLY_MV` in `src/main.cpp`; set it to 0 if the D1 mini is powered from USB instead).

## Setup

Uses [PlatformIO](https://platformio.org/), installed in a project-local venv:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
source .venv/bin/activate
```

Wi-Fi credentials, and optionally a web password, go in `include/secrets.h` (git-ignored):

```sh
cp include/secrets.example.h include/secrets.h   # then edit it
```

With no credentials, or if the network can't be reached within 15 s, the board starts
its own hotspot **LabPSU** (password `labpsu123`), page at http://192.168.4.1.

## Build & flash

```sh
pio run                              # build
pio run -e d1_mini_ota -t upload     # flash over Wi-Fi (board must be on the network)
pio run -e d1_mini -t upload         # flash over USB
pio device monitor                   # serial console, 115200 baud
```

### USB flashing on this board

- The board's USB chip is a **CH9102** and shows up as `/dev/ttyACM0` (not `ttyUSB`).
- Many USB-C cables are charge-only: if `ls /dev | grep ttyACM` shows nothing after
  plugging in, try another cable.
- **Auto-reset doesn't work** on this clone, so enter the bootloader by hand:
  connect **D3 to G**, tap **RST**, run the upload, then remove the wire and tap RST.
- Fallback: a CH341A in UART mode (TXD → RX, RXD → TX, GND → G) with the same D3 trick.
  If the port echoes back what you send, TX and RX are shorted together.

OTA avoids all of this once the board is on Wi-Fi. The output is switched off
automatically during an OTA update.

## Web page

http://labpsu.local (or the IP printed at boot / by the `wifi` command):

- live voltage, current, power, board temperature, with the set voltage and the drop under load
- output ON/OFF
- chart of voltage and current: live 2 minutes, or 10 min / 1 h / the whole log (hover for values)
- counters: on-time, mAh, Wh; output timer (off after N minutes and/or N mAh) and
  end-of-charge cutoff (off once the current stays below N mA for N seconds)
- PPS and AVS voltage + current (shown when the charger supports them)
- one-tap fixed-voltage profiles
- charger & cable report: every profile with its power, max power, PPS/AVS/EPR support,
  cable rating and measured cable resistance (see below)
- protection: current, voltage and power cutoffs, soft start
- measurement log, downloadable as CSV

Set `WEB_PASSWORD` in `secrets.h` to require a login (user `admin`, or `WEB_USER`).
Without it, anyone on the network can control the supply.

Settings (cutoffs, soft start, timer, log interval, last setpoint) are saved to
flash 3 s after a change and restored at boot. The output always starts off.

### JSON API

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

## Serial commands

| Command          | Effect                                              |
|------------------|-----------------------------------------------------|
| `help`           | list commands                                       |
| `wifi`           | network status and web address                      |
| `scan`           | scan the I²C bus                                    |
| `profile`        | list the charger's PDOs                             |
| `stat`           | measurements, settings, counters, last event        |
| `on` / `off`     | output switch                                       |
| `set <mV> [mA]`  | any voltage: picks PPS, fixed or AVS                |
| `fix <idx> [mA]` | request a fixed PDO                                 |
| `pps <mV> [mA]`  | request a PPS voltage (PPS charger, up to 21 V)     |
| `avs <mV> [mA]`  | request an AVS voltage (EPR charger + 5 A cable, up to 48 V) |
| `ocp <mA>`       | over-current cutoff                                 |
| `ovp <mV>`       | over-voltage cutoff (0 = off)                       |
| `opp <mW>`       | over-power cutoff (0 = off)                         |
| `clear`          | reset a tripped cutoff                              |
| `ramp <ms>`      | soft start duration (0 = off)                       |
| `timer <s>`      | switch off after this much on-time (0 = off)        |
| `mah <mAh>`      | switch off after this much charge (0 = off)         |
| `endcharge <mA> [s]` | switch off below <mA> for [s] seconds (0 = off)  |
| `pd`             | charger and cable report                            |
| `zero`           | reset the counters                                  |
| `log <s>`        | log interval (clears the log)                       |

Omitting `[mA]` uses the charger's maximum.

## Charger & cable report

The board doubles as a basic USB PD tester: plug in a charger and the page (or `pd` on
serial) lists what it offers.

- **Profiles** come from the AP33772S, which stores up to 7 standard and 6 EPR profiles.
  Currents are in 250 mA steps, as the chip reports them. The chip can't send arbitrary PD
  messages, so manufacturer IDs and other extended details aren't available.
- **Cable rating** is inferred: a charger only offers 5 A once it has read a 5 A e-marker in
  the cable, and only offers EPR profiles over an EPR cable. With 3 A profiles only, the
  limit could be the charger or the cable. The AP33772S is a sink and can't read the
  e-marker itself.
- **Cable resistance** is estimated from the voltage drop under load at the same setpoint:
  the charger regulates at its own connector and the INA238 measures at our input. It needs
  more than 0.3 A of load and includes the connectors. Chargers that compensate for cable
  drop make it read low.

## Safety

- Output is **off** at boot and during OTA updates.
- PPS and AVS requests outside the charger's advertised range are rejected. The PPS current
  limit (1 A minimum) is enforced by the charger; AVS and fixed profiles have none, so rely
  on the current cutoff.
- **Cutoffs** (current, default 5.1 A; voltage; power) work in hardware: the INA238 ALERT
  pin drives a MOSFET that pulls the output switch's enable (SW_EN) low, so the output is cut
  within a few ms of a single reading over the limit, without the ESP8266. The firmware also
  checks the averaged readings every 20 ms as a backup, and reports either kind of trip. The
  output stays off until reset (`clear` or the web button). Requests above the voltage
  cutoff are refused. The voltage cutoff applies to the charger side (VBUS in).
- **Soft start** (PPS/AVS only): the output switches on at the lowest allowed voltage and
  steps up to the setpoint over the set time. Setpoint changes while on are ramped too.
- **Timer / charge limit**: the output switches off when the on-time or mAh counter reaches
  the limit, and won't switch back on until the counters are reset or the limit is raised.
- **Log**: averages over the interval, 3000 entries kept in RAM (lost on reboot). Times are
  seconds since boot, plus Unix time when the board is on Wi-Fi and has reached an NTP server.

## Tested with

Charger advertising 5/9/12/15 V @ 3 A, 20 V @ 5 A, PPS 3.3–21 V @ 3 A.

## References

- [AP33772S Arduino library](https://github.com/CentyLab/AP33772S-CentyLab)
- [RotoPD Pro on Hackaday.io](https://hackaday.io/project/204943-rotopd-pro-usb-c-240w-breakout-i2c)
