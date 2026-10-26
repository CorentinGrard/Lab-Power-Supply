# Lab power supply

USB-C PD bench supply: a Wemos **D1 mini (ESP8266)** controls a **CentyLab RotoPD Pro**
(AP33772S PD 3.1 sink + INA238 power monitor) over I²C.

## Wiring

| D1 mini     | RotoPD Pro |
|-------------|------------|
| D2 (GPIO4)  | SDA        |
| D1 (GPIO5)  | SCL        |
| 3V3         | 3.3V In    |
| GND         | GND        |

I²C addresses: `0x40` INA238, `0x52` AP33772S.

## Build & flash

Uses [PlatformIO](https://platformio.org/), installed in a project-local venv:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
source .venv/bin/activate
```

Then:

```sh
pio run                  # build
pio run -t upload        # flash over USB
pio device monitor       # serial console, 115200 baud
```

## Serial commands

| Command          | Effect                                              |
|------------------|-----------------------------------------------------|
| `help`           | list commands                                       |
| `scan`           | scan the I²C bus                                    |
| `profile`        | list the charger's PDOs                             |
| `stat`           | bus voltage, current, power, temperature            |
| `on` / `off`     | output switch                                       |
| `fix <idx> <mA>` | request a fixed PDO                                 |
| `pps <mV> <mA>`  | request a PPS voltage (PPS charger, up to 21 V)     |
| `avs <mV> <mA>`  | request an AVS voltage (EPR charger + 5 A cable, up to 48 V) |
| `ocp <mA>`       | INA238 overcurrent limit                            |
| `clear`          | reset a tripped overcurrent alert                   |

Output is **off** at boot.

## References

- [AP33772S Arduino library](https://github.com/CentyLab/AP33772S-CentyLab)
- [RotoPD Pro on Hackaday.io](https://hackaday.io/project/204943-rotopd-pro-usb-c-240w-breakout-i2c)
