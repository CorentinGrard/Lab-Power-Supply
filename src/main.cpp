// Lab power supply: Wemos D1 mini (ESP8266) driving a CentyLab RotoPD Pro.
// Control it from the web page (http://labpsu.local) or the serial monitor (115200 baud, type "help").

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <AP33772S.h>
#include <INA238.h>
#include "web_page.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// D1 mini I2C pins
constexpr uint8_t PIN_SDA = D2; // GPIO4
constexpr uint8_t PIN_SCL = D1; // GPIO5

constexpr char HOSTNAME[] = "labpsu";
constexpr char AP_SSID[] = "LabPSU";        // Fallback hotspot when Wi-Fi is not configured or unreachable
constexpr char AP_PASSWORD[] = "labpsu123"; // WPA2 needs at least 8 characters
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t OCP_CHECK_MS = 20;

AP33772S usbpd;
INA238 ina238(0x40);
ESP8266WebServer server(80);

// ---------------------------------------------------------------------------
// Charger profiles (PDOs), decoded from the AP33772S registers.
// The library keeps its copy private, so we read them ourselves.

enum class PdoType : uint8_t { None, Fixed, PPS, AVS };

struct Pdo {
  PdoType type = PdoType::None;
  uint16_t minMv = 0;
  uint16_t maxMv = 0; // Fixed: the voltage
  uint16_t maxMa = 0;
};

constexpr int NUM_PDOS = 13; // Index 1-7 SPR, 8-13 EPR (1-based, like the library)
Pdo pdos[NUM_PDOS + 1];

uint16_t currentCodeToMa(uint8_t code) {
  if (code == 0) return 1000; // "less than 1.25 A"
  if (code >= 15) return 5000;
  return 1000 + 250 * code;
}

// The library rejects 5000 mA (it maps to code 16), so cap requests just below.
int libSafeMa(int ma) { return ma >= 5000 ? 4999 : ma; }

void readPdos() {
  Wire.beginTransmission(AP33772S_ADDRESS);
  Wire.write(CMD_SRCPDO);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)AP33772S_ADDRESS, (uint8_t)(NUM_PDOS * 2));

  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    uint16_t raw = Wire.read();
    raw |= Wire.read() << 8;
    Pdo &p = pdos[idx];
    p = Pdo();
    if (raw == 0 || raw == 0xFFFF) continue;

    bool epr = idx >= 8;
    bool adjustable = raw & (1 << 14);
    uint8_t vmaxRaw = raw & 0xFF;
    uint8_t vminRaw = (raw >> 8) & 0x03;
    p.maxMa = currentCodeToMa((raw >> 10) & 0x0F);
    p.maxMv = vmaxRaw * (epr ? 200 : 100);

    if (!adjustable) {
      p.type = PdoType::Fixed;
      p.minMv = p.maxMv;
    } else if (!epr) {
      p.type = PdoType::PPS;
      p.minMv = vminRaw == 2 ? 5000 : 3300;
    } else {
      p.type = PdoType::AVS;
      p.minMv = 15000;
    }
  }
}

bool havePdos() {
  for (int idx = 1; idx <= NUM_PDOS; idx++)
    if (pdos[idx].type != PdoType::None) return true;
  return false;
}

// When the D1 mini is powered by the RotoPD, both boot together and the charger's
// profiles may not be ready at our first read: keep trying until they show up.
void retryPdos() {
  constexpr uint32_t RETRY_MS = 2000;
  static uint32_t last = 0;
  if (havePdos() || millis() - last < RETRY_MS) return;
  last = millis();
  usbpd.begin();
  readPdos();
  if (havePdos()) Serial.println("Charger profiles found");
}

int findPps() {
  for (int idx = 1; idx <= 7; idx++)
    if (pdos[idx].type == PdoType::PPS) return idx;
  return -1;
}

// ---------------------------------------------------------------------------
// Supply state and actions (shared by the web API and the serial commands).
// Each action returns nullptr on success, or an error message.

enum class Mode : uint8_t { Default, Fixed, PPS };

struct State {
  bool outputOn = false;
  bool tripped = false;
  Mode mode = Mode::Default;
  int setMv = 5000;
  int setMa = 0;
  int ocpMa = 5100;
} st;

void setOutput(bool on) {
  if (on && st.tripped) return;
  usbpd.setOutput(on ? 1 : 0);
  st.outputOn = on;
}

const char *requestFixed(int idx, int ma) {
  if (idx < 1 || idx > 7 || pdos[idx].type != PdoType::Fixed) return "Not a fixed profile";
  if (ma <= 0 || ma > pdos[idx].maxMa) ma = pdos[idx].maxMa;
  usbpd.setFixPDO(idx, libSafeMa(ma));
  st.mode = Mode::Fixed;
  st.setMv = pdos[idx].maxMv;
  st.setMa = ma;
  return nullptr;
}

const char *requestPps(int mv, int ma) {
  int idx = findPps();
  if (idx < 0) return "Charger has no PPS profile";
  const Pdo &p = pdos[idx];
  if (mv < p.minMv || mv > p.maxMv) return "Voltage outside the charger's PPS range";
  if (ma < 1000 || ma > p.maxMa) return "Current limit outside 1 A .. charger maximum";
  usbpd.setPPSPDO(idx, mv, libSafeMa(ma));
  st.mode = Mode::PPS;
  st.setMv = mv;
  st.setMa = ma;
  return nullptr;
}

const char *setOcp(int ma) {
  if (ma < 100 || ma > 7000) return "Cutoff must be 100 .. 7000 mA";
  st.ocpMa = ma;
  ina238.setOverCurrentLimit(ma);
  return nullptr;
}

void clearTrip() {
  ina238.getDiagnoseAlert(); // Reading DIAG clears the latched INA238 alert
  st.tripped = false;
}

// Software over-current cutoff: switch the output off as soon as the measured
// current goes over the limit. The INA238 alert alone does not cut the output.
void checkOverCurrent() {
  static uint32_t last = 0;
  if (millis() - last < OCP_CHECK_MS) return;
  last = millis();
  if (!st.outputOn) return;
  float ma = fabsf(ina238.getMilliAmpere());
  if (ma > st.ocpMa) {
    usbpd.setOutput(0);
    st.outputOn = false;
    st.tripped = true;
    Serial.printf("Over-current cutoff: %.0f mA > %d mA, output OFF\n", ma, st.ocpMa);
  }
}

// ---------------------------------------------------------------------------
// Hardware init

void initRotoPD() {
  usbpd.clearConfig(AP33772_CONFIG::UVP_EN); // Required on the Pro: stops UVP from issuing a hard reset
  usbpd.begin();                             // Read the charger's PDOs (library copy)
  readPdos();                                // ...and our own copy for the web page
  setOutput(false);                          // Output off at boot

  if (!ina238.begin()) {
    Serial.println("INA238 not found");
  }
  ina238.setADCRange(1);
  ina238.setMaxCurrentShunt(7, 0.005); // 5 mOhm shunt, per RotoPD Pro schematic
  ina238.setShuntVoltageConversionTime(INA238_150_us);
  ina238.setAverage(INA238_16_SAMPLES);
  ina238.setOverCurrentLimit(st.ocpMa);
  ina238.setDiagnoseAlertBit(INA238_DIAG_ALERT_LATCH);
}

void initWifi() {
  WiFi.persistent(false); // Don't wear the flash with credentials
  WiFi.hostname(HOSTNAME);

  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: http://%s.local  (http://%s)\n", HOSTNAME,
                  WiFi.localIP().toString().c_str());
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("Hotspot \"%s\" (password %s): http://%s\n", AP_SSID, AP_PASSWORD,
                  WiFi.softAPIP().toString().c_str());
  }
}

void initOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([] {
    setOutput(false); // Never leave the output on while reflashing
    Serial.println("OTA update started, output OFF");
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin(); // Also starts mDNS (labpsu.local)
  MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------------------
// Web API

void reply(const char *err) {
  if (err) server.send(400, "text/plain", err);
  else server.send(200, "text/plain", "OK");
}

const char *modeName() {
  switch (st.mode) {
    case Mode::Fixed: return "fixed";
    case Mode::PPS: return "pps";
    default: return "default";
  }
}

void handleStatus() {
  char buf[256];
  snprintf(buf, sizeof buf,
           "{\"v\":%.3f,\"i\":%.3f,\"p\":%.3f,\"t\":%.1f,\"on\":%s,\"tripped\":%s,"
           "\"mode\":\"%s\",\"set_mv\":%d,\"set_ma\":%d,\"ocp_ma\":%d,\"rssi\":%d}",
           ina238.getBusVoltage(), ina238.getAmpere(), ina238.getWatt(), ina238.getTemperature(),
           st.outputOn ? "true" : "false", st.tripped ? "true" : "false", modeName(),
           st.setMv, st.setMa, st.ocpMa, WiFi.RSSI());
  server.send(200, "application/json", buf);
}

void handlePdos() {
  String json = "[";
  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    const Pdo &p = pdos[idx];
    if (p.type == PdoType::None) continue;
    const char *type = p.type == PdoType::Fixed ? "fixed" : p.type == PdoType::PPS ? "pps" : "avs";
    char buf[96];
    snprintf(buf, sizeof buf, "%s{\"idx\":%d,\"type\":\"%s\",\"mv\":%u,\"min_mv\":%u,\"max_mv\":%u,\"ma\":%u}",
             json.length() > 1 ? "," : "", idx, type, p.maxMv, p.minMv, p.maxMv, p.maxMa);
    json += buf;
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSet() {
  String mode = server.arg("mode");
  int ma = server.arg("ma").toInt();
  if (mode == "pps") reply(requestPps(server.arg("mv").toInt(), ma));
  else if (mode == "fixed") reply(requestFixed(server.arg("idx").toInt(), ma));
  else reply("mode must be pps or fixed");
}

void handleOutput() {
  bool on = server.arg("on") == "1";
  if (on && st.tripped) return reply("Over-current cutoff tripped, reset it first");
  setOutput(on);
  reply(nullptr);
}

void initServer() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", WEB_PAGE); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pdos", HTTP_GET, handlePdos);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/output", HTTP_POST, handleOutput);
  server.on("/api/ocp", HTTP_POST, [] { reply(setOcp(server.arg("ma").toInt())); });
  server.on("/api/clear", HTTP_POST, [] { clearTrip(); reply(nullptr); });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ---------------------------------------------------------------------------
// Serial commands

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
  Serial.printf("Bus %.3f V  |  %.3f A  |  %.3f W  |  %.1f C  |  output %s%s\n",
                ina238.getBusVoltage(), ina238.getAmpere(), ina238.getWatt(),
                ina238.getTemperature(), st.outputOn ? "ON" : "OFF",
                st.tripped ? " (over-current tripped)" : "");
}

void printWifi() {
  if (WiFi.getMode() == WIFI_AP)
    Serial.printf("Hotspot \"%s\": http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  else
    Serial.printf("Wi-Fi %s, http://%s.local (http://%s), RSSI %d dBm\n",
                  WiFi.isConnected() ? "connected" : "disconnected", HOSTNAME,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void printHelp() {
  Serial.println(F(
      "Commands:\n"
      "  help                 this list\n"
      "  wifi                 network status / web address\n"
      "  scan                 scan the I2C bus\n"
      "  profile              list the charger's PDOs\n"
      "  stat                 voltage / current / power / temperature\n"
      "  on | off             output switch\n"
      "  fix <idx> <mA>       request fixed PDO <idx>\n"
      "  pps <mV> <mA>        request a PPS voltage (needs a PPS charger)\n"
      "  avs <mV> <mA>        request an AVS voltage (needs an EPR charger + 5A cable)\n"
      "  ocp <mA>             over-current cutoff\n"
      "  clear                reset a tripped over-current cutoff"));
}

void printResult(const char *err, const char *ok) { Serial.println(err ? err : ok); }

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
  } else if (c == "wifi") {
    printWifi();
  } else if (c == "scan") {
    i2cScan();
  } else if (c == "profile") {
    usbpd.displayProfiles();
  } else if (c == "stat") {
    printStatus();
  } else if (c == "on") {
    setOutput(true);
    Serial.println(st.tripped ? "Over-current cutoff tripped, type 'clear' first" : "Output ON");
  } else if (c == "off") {
    setOutput(false);
    Serial.println("Output OFF");
  } else if (c == "fix" && n == 3) {
    printResult(requestFixed(a, b), "Fixed PDO requested");
  } else if (c == "pps" && n == 3) {
    printResult(requestPps(a, b), "PPS requested");
  } else if (c == "avs" && n == 3) {
    int idx = usbpd.getAVSIndex();
    if (idx < 0) { Serial.println("Charger has no AVS profile"); return; }
    usbpd.setAVSPDO(idx, a, b);
    Serial.printf("Requested AVS %ld mV, %ld mA\n", a, b);
  } else if (c == "ocp" && n == 2) {
    printResult(setOcp(a), "Over-current cutoff set");
  } else if (c == "clear") {
    clearTrip();
    Serial.println("Cutoff reset");
  } else {
    Serial.println("Unknown or incomplete command, type 'help'");
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100); // Short read timeout so serial input never stalls the over-current check
  delay(1000); // Let the PD controller finish negotiating

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setClockStretchLimit(1500); // ESP8266 software I2C needs this for the AP33772S

  Serial.println("\nRotoPD Pro lab supply");
  i2cScan();
  initRotoPD();
  initWifi();
  initOta();
  initServer();
  printHelp();
}

void loop() {
  checkOverCurrent();
  retryPdos();
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }
}
