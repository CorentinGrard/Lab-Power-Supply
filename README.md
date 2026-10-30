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
gets them (the RotoPD may still be negotiating when the D1 mini starts). Reload the web
page once they're found. After changing chargers, press RST on the D1 mini.

## Setup

Uses [PlatformIO](https://platformio.org/), installed in a project-local venv:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
source .venv/bin/activate
```

Wi-Fi credentials go in `include/secrets.h` (git-ignored):

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

- live voltage, current, power, board temperature
- output ON/OFF
- PPS voltage and current limit (shown when the charger supports PPS)
- one-tap fixed-voltage profiles
- over-current cutoff setting and reset

There is no login: anyone on the network can control the supply.

JSON API used by the page: `GET /api/status`, `GET /api/pdos`,
`POST /api/set?mode=pps&mv=&ma=` or `?mode=fixed&idx=&ma=`, `POST /api/output?on=0|1`,
`POST /api/ocp?ma=`, `POST /api/clear`.

## Serial commands

| Command          | Effect                                              |
|------------------|-----------------------------------------------------|
| `help`           | list commands                                       |
| `wifi`           | network status and web address                      |
| `scan`           | scan the I²C bus                                    |
| `profile`        | list the charger's PDOs                             |
| `stat`           | bus voltage, current, power, temperature, output state |
| `on` / `off`     | output switch                                       |
| `fix <idx> <mA>` | request a fixed PDO                                 |
| `pps <mV> <mA>`  | request a PPS voltage (PPS charger, up to 21 V)     |
| `avs <mV> <mA>`  | request an AVS voltage (EPR charger + 5 A cable, up to 48 V) |
| `ocp <mA>`       | over-current cutoff                                 |
| `clear`          | reset a tripped over-current cutoff                 |

## Safety

- Output is **off** at boot and during OTA updates.
- PPS requests outside the charger's advertised range are rejected. The PPS current limit
  (1 A minimum) is enforced by the charger.
- **Over-current cutoff**: the firmware polls the INA238 every 20 ms and switches the output
  off above the limit (default 5.1 A). It stays off until reset (`clear` or the web button).

## Tested with

Charger advertising 5/9/12/15 V @ 3 A, 20 V @ 5 A, PPS 3.3–21 V @ 3 A.

## References

- [AP33772S Arduino library](https://github.com/CentyLab/AP33772S-CentyLab)
- [RotoPD Pro on Hackaday.io](https://hackaday.io/project/204943-rotopd-pro-usb-c-240w-breakout-i2c)
