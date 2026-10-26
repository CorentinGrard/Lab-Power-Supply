// Bring-up firmware: control the RotoPD Pro from the serial monitor.
// Based on the RotoPDPro_SerialCommand example, adapted for the D1 mini.
// Type "help" in the serial monitor (115200 baud) to list commands.

#include <Arduino.h>
#include <Wire.h>
#include <AP33772S.h>
#include <INA238.h>

// D1 mini I2C pins
constexpr uint8_t PIN_SDA = D2; // GPIO4
constexpr uint8_t PIN_SCL = D1; // GPIO5

AP33772S usbpd;
INA238 ina238(0x40);

void initRotoPD() {
  usbpd.clearConfig(AP33772_CONFIG::UVP_EN); // Required on the Pro: stops UVP from issuing a hard reset
  usbpd.begin();                             // Read the charger's PDOs
  usbpd.setOutput(0);                        // Output off at boot

  if (!ina238.begin()) {
    Serial.println("INA238 not found");
  }
  ina238.setADCRange(1);
  ina238.setMaxCurrentShunt(7, 0.005); // 5 mOhm shunt, per RotoPD Pro schematic
  ina238.setShuntVoltageConversionTime(INA238_150_us);
  ina238.setAverage(INA238_16_SAMPLES);
  ina238.setOverCurrentLimit(5100);                    // mA
  ina238.setDiagnoseAlertBit(INA238_DIAG_ALERT_LATCH); // Breaker stays tripped until "clear"
}

void i2cScan() {
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X\n", addr);
      found++;
    }
  }
  Serial.printf("%d device(s) found (expect 0x40 INA238, 0x52 AP33772S)\n", found);
}

void printStatus() {
  Serial.printf("Bus %.3f V  |  %.3f A  |  %.3f W  |  %.1f C\n",
                ina238.getBusVoltage(), ina238.getAmpere(),
                ina238.getWatt(), ina238.getTemperature());
}

void printHelp() {
  Serial.println(F(
      "Commands:\n"
      "  help                 this list\n"
      "  scan                 scan the I2C bus\n"
      "  profile              list the charger's PDOs\n"
      "  stat                 voltage / current / power / temperature\n"
      "  on | off             output switch\n"
      "  fix <idx> <mA>       request fixed PDO <idx>\n"
      "  pps <mV> <mA>        request a PPS voltage (needs a PPS charger)\n"
      "  avs <mV> <mA>        request an AVS voltage (needs an EPR charger + 5A cable)\n"
      "  ocp <mA>             INA238 overcurrent limit\n"
      "  clear                reset a tripped overcurrent alert"));
}

void handleCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;

  char cmd[16] = {0};
  long a = -1, b = -1;
  int n = sscanf(line.c_str(), "%15s %ld %ld", cmd, &a, &b);
  String c(cmd);
  c.toLowerCase();

  if (c == "help") {
    printHelp();
  } else if (c == "scan") {
    i2cScan();
  } else if (c == "profile") {
    usbpd.displayProfiles();
  } else if (c == "stat") {
    printStatus();
  } else if (c == "on") {
    usbpd.setOutput(1);
    Serial.println("Output ON");
  } else if (c == "off") {
    usbpd.setOutput(0);
    Serial.println("Output OFF");
  } else if (c == "fix" && n == 3) {
    usbpd.setFixPDO(a, b);
    Serial.printf("Requested fixed PDO %ld, %ld mA\n", a, b);
  } else if (c == "pps" && n == 3) {
    int idx = usbpd.getPPSIndex();
    if (idx < 0) { Serial.println("Charger has no PPS profile"); return; }
    usbpd.setPPSPDO(idx, a, b);
    Serial.printf("Requested PPS %ld mV, %ld mA\n", a, b);
  } else if (c == "avs" && n == 3) {
    int idx = usbpd.getAVSIndex();
    if (idx < 0) { Serial.println("Charger has no AVS profile"); return; }
    usbpd.setAVSPDO(idx, a, b);
    Serial.printf("Requested AVS %ld mV, %ld mA\n", a, b);
  } else if (c == "ocp" && n == 2) {
    ina238.setOverCurrentLimit(a);
    Serial.printf("Overcurrent limit %ld mA\n", a);
  } else if (c == "clear") {
    ina238.getDiagnoseAlert(); // Reading DIAG clears the latched alert
    Serial.println("Alert cleared");
  } else {
    Serial.println("Unknown or incomplete command, type 'help'");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Let the PD controller finish negotiating

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setClockStretchLimit(1500); // ESP8266 software I2C needs this for the AP33772S

  Serial.println("\nRotoPD Pro lab supply");
  i2cScan();
  initRotoPD();
  printHelp();
}

void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }
}
