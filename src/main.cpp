// Lab power supply: Wemos D1 mini (ESP8266) driving a CentyLab RotoPD Pro.
// Control it from the web page (http://labpsu.local) or the serial monitor (115200 baud, type "help").

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <time.h>
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

// Defaults for settings that older secrets.h files don't have
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD ""
#endif

// D1 mini I2C pins
constexpr uint8_t PIN_SDA = D2; // GPIO4
constexpr uint8_t PIN_SCL = D1; // GPIO5

constexpr char HOSTNAME[] = "labpsu";
constexpr char AP_SSID[] = "LabPSU";        // Fallback hotspot when Wi-Fi is not configured or unreachable
constexpr char AP_PASSWORD[] = "labpsu123"; // WPA2 needs at least 8 characters
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t SAMPLE_MS = 20;        // Measurement + protection loop period
constexpr uint32_t RAMP_STEP_MS = 100;    // Soft start: time between voltage steps
constexpr uint32_t RAMP_SETTLE_MS = 300;  // Soft start: wait for the start voltage before switching on
constexpr uint32_t SAVE_DELAY_MS = 3000;  // Settings are written to flash this long after the last change
constexpr uint32_t CHARGER_CHECK_MS = 1000;
// The D1 mini runs from the RotoPD's 5V Out, which follows the charger voltage
// down: below about 4.2 V the D1 mini browns out and reboots. PPS/AVS requests
// are kept above this. Set it to 0 if the D1 mini is powered from USB instead.
constexpr int MIN_SUPPLY_MV = 4500;

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
  bool operator!=(const Pdo &o) const {
    return type != o.type || minMv != o.minMv || maxMv != o.maxMv || maxMa != o.maxMa;
  }
};

constexpr int NUM_PDOS = 13; // Index 1-7 SPR, 8-13 EPR (1-based, like the library)
Pdo pdos[NUM_PDOS + 1];
uint16_t profileGen = 0; // Bumped when the profiles change, so the web page reloads them

uint16_t currentCodeToMa(uint8_t code) {
  if (code == 0) return 1000; // "less than 1.25 A"
  if (code >= 15) return 5000;
  return 1000 + 250 * code;
}

// The library rejects 5000 mA (it maps to code 16), so cap requests just below.
int libSafeMa(int ma) { return ma >= 5000 ? 4999 : ma; }

void readPdos(Pdo *out) {
  Wire.beginTransmission(AP33772S_ADDRESS);
  Wire.write(CMD_SRCPDO);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)AP33772S_ADDRESS, (uint8_t)(NUM_PDOS * 2));

  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    uint16_t raw = Wire.read();
    raw |= Wire.read() << 8;
    Pdo &p = out[idx];
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

int findPdo(PdoType type) {
  for (int idx = 1; idx <= NUM_PDOS; idx++)
    if (pdos[idx].type == type) return idx;
  return -1;
}

int findFixed(int mv) {
  for (int idx = 1; idx <= 7; idx++)
    if (pdos[idx].type == PdoType::Fixed && abs(pdos[idx].maxMv - mv) <= 50) return idx;
  return -1;
}

// ---------------------------------------------------------------------------
// Settings, saved to flash (EEPROM emulation) a few seconds after each change.

enum class Mode : uint8_t { Default, Fixed, PPS, AVS };

constexpr uint32_t CONFIG_MAGIC = 0x50535501; // Change the last byte when the layout changes

struct Config {
  uint32_t magic = CONFIG_MAGIC;
  Mode mode = Mode::Default; // Last setpoint, restored at boot (the output still starts off)
  uint16_t setMv = 5000;
  uint16_t setMa = 0;
  uint16_t ocpMa = 5100;
  uint16_t ovpMv = 0;    // 0 = off
  uint32_t oppMw = 0;    // 0 = off
  uint16_t rampMs = 0;   // Soft start duration, 0 = off
  uint32_t offAfterS = 0;    // Output timer, 0 = off
  uint32_t offAfterMah = 0;  // Charge limit, 0 = off
  uint16_t logIntervalS = 1;
  // New fields go at the end, so older saved settings stay readable (checked in loadConfig)
  uint16_t endMa = 0; // End of charge: switch off below this current... (0 = off)
  uint16_t endS = 30; // ...held for this long
} cfg;

static_assert(sizeof(Config) <= 512, "Config too big for the EEPROM area");

uint32_t dirtyAt = 0;
bool dirty = false;

void markDirty() {
  dirty = true;
  dirtyAt = millis();
}

void loadConfig() {
  EEPROM.begin(512);
  Config saved;
  EEPROM.get(0, saved);
  if (saved.magic == CONFIG_MAGIC) cfg = saved;
  else Serial.println("No saved settings, using defaults");
  // Fields added later may hold leftovers from an older layout
  if (cfg.endMa > 5000) cfg.endMa = 0;
  if (cfg.endS < 1 || cfg.endS > 3600) cfg.endS = 30;
}

void saveConfigIfDirty() {
  if (!dirty || millis() - dirtyAt < SAVE_DELAY_MS) return;
  dirty = false;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Live state

enum class Trip : uint8_t { None, OCP, OVP, OPP };

struct State {
  bool outputOn = false;
  Trip trip = Trip::None;
  char note[80] = ""; // Last automatic action (cutoff, timer...), shown on the web page
  float v = 0, i = 0, p = 0; // Latest measurement
  double mah = 0, mwh = 0;   // Counters, accumulate while the output is on
  uint32_t onMs = 0;
  uint32_t lowMs = 0;        // How long the current has been below the end-of-charge level
} st;

// Cable resistance estimate. The charger holds its voltage at its own connector and
// the INA238 measures at our input, so the extra drop under load is the cable
// (plus connectors): R = (V no load - V loaded) / I, at the same setpoint.
struct Cable {
  float v0 = 0;         // No-load voltage at the current setpoint
  bool v0ok = false;
  uint32_t settleAt = 0; // Ignore readings until the charger has settled after a request
  float r = -1;          // Ohms, -1 = not measured yet
  float atA = 0;         // Current of the last measurement
} cable;

void newSetpoint() {
  cable.v0ok = false;
  cable.settleAt = millis() + 1500;
}

struct Ramp {
  bool pendingOn = false; // Soft start: waiting for the start voltage before switching on
  bool active = false;    // Stepping towards cfg.setMv
  int curMv = 0;
  int stepMv = 0;
  uint32_t next = 0;
} ramp;

const char *tripName() {
  switch (st.trip) {
    case Trip::OCP: return "ocp";
    case Trip::OVP: return "ovp";
    case Trip::OPP: return "opp";
    default: return "";
  }
}

const char *modeName() {
  switch (cfg.mode) {
    case Mode::Fixed: return "fixed";
    case Mode::PPS: return "pps";
    case Mode::AVS: return "avs";
    default: return "default";
  }
}

bool isAdjustable(Mode m) { return m == Mode::PPS || m == Mode::AVS; }
PdoType pdoTypeOf(Mode m) { return m == Mode::AVS ? PdoType::AVS : PdoType::PPS; }
int grainMv(Mode m) { return m == Mode::AVS ? 200 : 100; } // Voltage step the request register supports
int minMvOf(const Pdo &p) { return max((int)p.minMv, MIN_SUPPLY_MV); }

// ---------------------------------------------------------------------------
// Hardware cutoff. On the RotoPD Pro the INA238 ALERT pin drives a MOSFET that
// pulls the output switch enable (SW_EN, LM74502) low, so a limit crossing cuts
// the output within a few ms without the ESP8266. The alert is latched until
// the DIAG_ALRT register is read. Alert polarity must stay at its default
// (active low), or the output would be held off.

constexpr uint16_t DIAG_POL = 1 << 2;    // Power over limit
constexpr uint16_t DIAG_BUSOL = 1 << 4;  // Bus over-voltage
constexpr uint16_t DIAG_SHNTOL = 1 << 6; // Shunt over-voltage = over-current
constexpr uint16_t DIAG_TRIPS = DIAG_POL | DIAG_BUSOL | DIAG_SHNTOL;

void applyHardwareLimits() {
  ina238.setOverCurrentLimit(cfg.ocpMa);
  ina238.setBusOvervoltageTH(cfg.ovpMv ? (uint16_t)(cfg.ovpMv / 3.125f) : 0x7FFF); // 3.125 mV/LSB
  float wPerLsb = 256 * 0.2f * ina238.getCurrentLSB(); // POWER_LIMIT LSB = 256 x power LSB
  uint32_t pl = cfg.oppMw ? (uint32_t)(cfg.oppMw / 1000.0f / wPerLsb) : 0xFFFF;
  ina238.setPowerOverLimitTH(pl > 0xFFFF ? 0xFFFF : pl);
}

// ---------------------------------------------------------------------------
// Output and setpoint actions (shared by the web API and the serial commands).
// Each action returns nullptr on success, or an error message.

// Send an adjustable request. Values were checked by the caller.
void sendAdjustable(Mode m, int mv, int ma) {
  newSetpoint();
  int idx = findPdo(pdoTypeOf(m));
  if (m == Mode::AVS) usbpd.setAVSPDO(idx, mv, libSafeMa(ma));
  else usbpd.setPPSPDO(idx, mv, libSafeMa(ma));
}

void cancelRamp() {
  bool wasRamping = ramp.active || ramp.pendingOn;
  ramp.active = ramp.pendingOn = false;
  // Leave the charger at the setpoint, not at an intermediate step
  if (wasRamping && isAdjustable(cfg.mode)) sendAdjustable(cfg.mode, cfg.setMv, cfg.setMa);
}

bool rampEnabled() { return cfg.rampMs > 0 && isAdjustable(cfg.mode); }

void startRamp(int fromMv) {
  int span = abs(cfg.setMv - fromMv);
  int steps = max(1, (int)(cfg.rampMs / RAMP_STEP_MS));
  int g = grainMv(cfg.mode);
  ramp.stepMv = max(g, (span / steps) / g * g);
  ramp.curMv = fromMv;
  ramp.active = true;
  ramp.next = millis();
}

bool limitReached() {
  return (cfg.offAfterS && st.onMs >= cfg.offAfterS * 1000) ||
         (cfg.offAfterMah && st.mah >= cfg.offAfterMah);
}

// Switch on. PWR_EN is still off here, so clearing a stale INA238 latch is safe.
void switchOn() {
  ina238.getDiagnoseAlert();
  st.lowMs = 0;
  usbpd.setOutput(1);
  st.outputOn = true;
}

void switchOff() {
  cancelRamp();
  usbpd.setOutput(0);
  st.outputOn = false;
}

const char *setOutput(bool on) {
  if (!on) {
    switchOff();
    return nullptr;
  }
  if (st.trip != Trip::None) return "A cutoff tripped, reset it first";
  if (limitReached()) return "Timer or charge limit reached: reset the counters or raise the limit";
  if (cfg.ovpMv && cfg.setMv > cfg.ovpMv) return "Set voltage is above the over-voltage cutoff";
  st.note[0] = 0;

  if (rampEnabled() && !st.outputOn) {
    // Soft start: go to the profile's lowest voltage, switch on, then step up
    int g = grainMv(cfg.mode);
    int startMv = (minMvOf(pdos[findPdo(pdoTypeOf(cfg.mode))]) + g - 1) / g * g;
    if (startMv < cfg.setMv) {
      sendAdjustable(cfg.mode, startMv, cfg.setMa);
      ramp.curMv = startMv;
      ramp.pendingOn = true;
      ramp.next = millis() + RAMP_SETTLE_MS;
      return nullptr;
    }
  }
  switchOn();
  return nullptr;
}

void rampTask() {
  if ((int32_t)(millis() - ramp.next) < 0) return;
  if (ramp.pendingOn) {
    ramp.pendingOn = false;
    switchOn();
    startRamp(ramp.curMv);
    ramp.next = millis() + RAMP_STEP_MS;
    return;
  }
  if (!ramp.active) return;
  int target = cfg.setMv;
  if (abs(target - ramp.curMv) <= ramp.stepMv) {
    ramp.curMv = target;
    ramp.active = false;
  } else {
    ramp.curMv += target > ramp.curMv ? ramp.stepMv : -ramp.stepMv;
  }
  sendAdjustable(cfg.mode, ramp.curMv, cfg.setMa);
  ramp.next = millis() + RAMP_STEP_MS;
}

const char *checkVoltage(int mv) {
  if (cfg.ovpMv && mv > cfg.ovpMv) return "Voltage is above the over-voltage cutoff";
  return nullptr;
}

const char *requestFixed(int idx, int ma) {
  if (idx < 1 || idx > 7 || pdos[idx].type != PdoType::Fixed) return "Not a fixed profile";
  if (const char *err = checkVoltage(pdos[idx].maxMv)) return err;
  if (ma <= 0 || ma > pdos[idx].maxMa) ma = pdos[idx].maxMa;
  ramp.active = ramp.pendingOn = false;
  newSetpoint();
  usbpd.setFixPDO(idx, libSafeMa(ma));
  cfg.mode = Mode::Fixed;
  cfg.setMv = pdos[idx].maxMv;
  cfg.setMa = ma;
  markDirty();
  return nullptr;
}

// PPS or AVS. ma = 0 means the charger's maximum.
const char *requestAdjustable(Mode m, int mv, int ma) {
  bool avs = m == Mode::AVS;
  int idx = findPdo(pdoTypeOf(m));
  if (idx < 0) return avs ? "Charger has no AVS profile" : "Charger has no PPS profile";
  const Pdo &p = pdos[idx];
  int g = grainMv(m);
  mv = (mv + g / 2) / g * g;
  if (mv < MIN_SUPPLY_MV) return "Too low: the D1 mini runs from 5V Out and would reboot";
  if (mv < p.minMv || mv > p.maxMv)
    return avs ? "Voltage outside the charger's AVS range" : "Voltage outside the charger's PPS range";
  if (ma == 0) ma = p.maxMa;
  if (ma < 1000 || ma > p.maxMa) return "Current limit outside 1 A .. charger maximum";
  if (const char *err = checkVoltage(mv)) return err;

  bool rampFrom = st.outputOn && cfg.rampMs > 0 && cfg.mode == m;
  int fromMv = ramp.active ? ramp.curMv : cfg.setMv;
  ramp.pendingOn = false;
  cfg.mode = m;
  cfg.setMv = mv;
  cfg.setMa = ma;
  markDirty();
  if (rampFrom) startRamp(fromMv);
  else {
    ramp.active = false;
    sendAdjustable(m, mv, ma);
  }
  return nullptr;
}

// Pick a profile for a voltage: PPS when it fits (it limits the current),
// else a matching fixed profile, else AVS.
const char *requestVoltage(int mv, int ma) {
  int pps = findPdo(PdoType::PPS);
  if (pps > 0 && mv >= minMvOf(pdos[pps]) && mv <= pdos[pps].maxMv && ma <= pdos[pps].maxMa)
    return requestAdjustable(Mode::PPS, mv, ma);
  int fixed = findFixed(mv);
  if (fixed > 0) return requestFixed(fixed, ma);
  int avs = findPdo(PdoType::AVS);
  if (avs > 0 && mv >= pdos[avs].minMv && mv <= pdos[avs].maxMv)
    return requestAdjustable(Mode::AVS, mv, ma);
  return "No charger profile can give this voltage";
}

// Re-apply the saved setpoint after boot or a charger change (output stays as is).
void restoreSetpoint() {
  const char *err = nullptr;
  switch (cfg.mode) {
    case Mode::Fixed: {
      int idx = findFixed(cfg.setMv);
      err = idx > 0 ? requestFixed(idx, cfg.setMa) : "no matching profile";
      break;
    }
    case Mode::PPS:
    case Mode::AVS: err = requestAdjustable(cfg.mode, cfg.setMv, cfg.setMa); break;
    default: return;
  }
  if (err) {
    Serial.printf("Saved setpoint not available on this charger (%s), using default 5 V\n", err);
    cfg.mode = Mode::Default;
    cfg.setMv = 5000;
    cfg.setMa = 0;
  } else {
    Serial.printf("Restored %s %d mV\n", modeName(), cfg.setMv);
  }
}

const char *setProtection(long ocpMa, long ovpMv, long oppMw) {
  if (ocpMa >= 0 && (ocpMa < 100 || ocpMa > 7000)) return "Current cutoff must be 100 .. 7000 mA";
  if (ovpMv > 0 && (ovpMv < 3000 || ovpMv > 50000)) return "Voltage cutoff must be 3 .. 50 V (0 = off)";
  if (oppMw > 0 && (oppMw < 500 || oppMw > 250000)) return "Power cutoff must be 0.5 .. 250 W (0 = off)";
  if (ocpMa >= 0) cfg.ocpMa = ocpMa;
  if (ovpMv >= 0) cfg.ovpMv = ovpMv;
  if (oppMw >= 0) cfg.oppMw = oppMw;
  applyHardwareLimits();
  markDirty();
  return nullptr;
}

const char *setRamp(long ms) {
  if (ms < 0 || ms > 10000) return "Soft start must be 0 .. 10000 ms";
  cfg.rampMs = ms;
  markDirty();
  return nullptr;
}

const char *setTimer(long offS, long offMah, long endMa = -1, long endS = -1) {
  if (offS > 864000) return "Timer must be at most 10 days";
  if (offMah > 1000000) return "Charge limit must be at most 1000 Ah";
  if (endMa > 5000) return "End-of-charge current must be at most 5000 mA";
  if (endS == 0 || endS > 3600) return "End-of-charge delay must be 1 .. 3600 s";
  if (offS >= 0) cfg.offAfterS = offS;
  if (offMah >= 0) cfg.offAfterMah = offMah;
  if (endMa >= 0) cfg.endMa = endMa;
  if (endS > 0) cfg.endS = endS;
  markDirty();
  return nullptr;
}

void resetCounters() {
  st.mah = st.mwh = 0;
  st.onMs = 0;
}

void clearTrip() {
  ina238.getDiagnoseAlert(); // Reading DIAG clears the latched INA238 alert
  st.trip = Trip::None;
  st.note[0] = 0;
}

// ---------------------------------------------------------------------------
// Measurement log: averages over cfg.logIntervalS, kept in RAM (lost on reboot).

constexpr int LOG_SIZE = 3000;
struct LogEntry {
  uint16_t mv;
  int16_t ma;
};
LogEntry logBuf[LOG_SIZE];
int logHead = 0, logCount = 0; // logHead = next slot to write
uint32_t logLastMs = 0;        // Time of the newest entry
double logSumV = 0, logSumI = 0;
uint32_t logSamples = 0;

const char *setLogInterval(long s) {
  if (s < 1 || s > 3600) return "Log interval must be 1 .. 3600 s";
  cfg.logIntervalS = s;
  logHead = logCount = 0;
  logSumV = logSumI = 0;
  logSamples = 0;
  logLastMs = millis();
  markDirty();
  return nullptr;
}

void logTask() {
  if (millis() - logLastMs < cfg.logIntervalS * 1000UL || logSamples == 0) return;
  logLastMs = millis();
  logBuf[logHead] = {(uint16_t)constrain(logSumV / logSamples * 1000, 0, 65535),
                     (int16_t)constrain(logSumI / logSamples * 1000, -32768, 32767)};
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  logSumV = logSumI = 0;
  logSamples = 0;
}

// ---------------------------------------------------------------------------
// Measurement and protection loop

void tripOff(Trip t, const char *fmt, float value, float limit) {
  switchOff();
  st.trip = t;
  snprintf(st.note, sizeof st.note, fmt, value, limit);
  Serial.printf("%s, output OFF\n", st.note);
}

void stopOff(const char *msg) {
  switchOff();
  snprintf(st.note, sizeof st.note, "%s", msg);
  Serial.printf("%s, output OFF\n", msg);
}

// After a hardware cutoff the current drops to zero while we still think the
// output is on. Reading DIAG_ALRT would release the latch and switch the output
// back on, so PWR_EN goes off first; if nothing tripped it goes back on (~1 ms gap).
void checkHardwareTrip() {
  usbpd.setOutput(0);
  uint16_t diag = ina238.getDiagnoseAlert();
  if (!(diag & DIAG_TRIPS)) {
    usbpd.setOutput(1);
    return;
  }
  switchOff();
  st.trip = diag & DIAG_SHNTOL ? Trip::OCP : diag & DIAG_BUSOL ? Trip::OVP : Trip::OPP;
  snprintf(st.note, sizeof st.note, "Hardware cutoff: %s",
           st.trip == Trip::OCP ? "over-current" : st.trip == Trip::OVP ? "over-voltage" : "over-power");
  Serial.printf("%s, output OFF\n", st.note);
}

void sampleTask() {
  static uint32_t last = 0;
  uint32_t now = millis();
  uint32_t dt = now - last;
  if (dt < SAMPLE_MS) return;
  last = now;
  if (dt > 10 * SAMPLE_MS) dt = SAMPLE_MS; // First call, or the loop was blocked

  st.v = ina238.getBusVoltage();
  st.i = ina238.getAmpere();
  st.p = st.v * st.i;
  logSumV += st.v;
  logSumI += st.i;
  logSamples++;

  if ((int32_t)(now - cable.settleAt) >= 0 && !ramp.active && !ramp.pendingOn) {
    float a = fabsf(st.i);
    if (a < 0.02f) {
      cable.v0 = cable.v0ok ? cable.v0 * 0.9f + st.v * 0.1f : st.v;
      cable.v0ok = true;
    } else if (a > 0.3f && cable.v0ok) {
      float r = (cable.v0 - st.v) / a;
      if (r > -0.05f && r < 2) {
        cable.r = cable.r < 0 ? r : cable.r * 0.95f + r * 0.05f;
        cable.atA = a;
      }
    }
  }

  if (!st.outputOn) return;
  st.mah += st.i * dt / 3600.0;
  st.mwh += st.p * dt / 3600.0;
  st.onMs += dt;

  // A hardware cutoff shows up as the current falling to zero under load
  static bool loaded = false;
  float ma = fabsf(st.i) * 1000;
  if (ma >= 50) loaded = true;
  else if (loaded && ma < 20) {
    loaded = false;
    checkHardwareTrip();
    if (!st.outputOn) return;
  }

  // Software cutoffs on the averaged readings: a backup for the hardware ones, and
  // they report the measured value.
  if (ma > cfg.ocpMa) return tripOff(Trip::OCP, "Over-current: %.2f A > %.2f A", ma / 1000, cfg.ocpMa / 1000.0f);
  if (cfg.ovpMv && st.v * 1000 > cfg.ovpMv)
    return tripOff(Trip::OVP, "Over-voltage: %.2f V > %.2f V", st.v, cfg.ovpMv / 1000.0f);
  if (cfg.oppMw && fabsf(st.p) * 1000 > cfg.oppMw)
    return tripOff(Trip::OPP, "Over-power: %.1f W > %.1f W", fabsf(st.p), cfg.oppMw / 1000.0f);
  if (cfg.offAfterS && st.onMs >= cfg.offAfterS * 1000) return stopOff("Timer finished");
  if (cfg.offAfterMah && st.mah >= cfg.offAfterMah) return stopOff("Charge limit reached");
  if (cfg.endMa && !ramp.active) {
    st.lowMs = ma < cfg.endMa ? st.lowMs + dt : 0;
    if (st.lowMs >= cfg.endS * 1000UL) {
      char msg[64];
      snprintf(msg, sizeof msg, "End of charge: below %u mA for %u s", cfg.endMa, cfg.endS);
      return stopOff(msg);
    }
  }
}

// ---------------------------------------------------------------------------
// Charger profiles: retry until found at boot, and notice a charger change.

void onNewProfiles(const Pdo *fresh) {
  memcpy(pdos, fresh, sizeof pdos);
  usbpd.begin(); // Refresh the library's copy too
  cable.r = -1;  // Possibly a different cable
  profileGen++;
  restoreSetpoint();
}

// When the D1 mini is powered by the RotoPD, both boot together and the charger's
// profiles may not be ready at our first read: keep trying until they show up.
void retryPdos() {
  constexpr uint32_t RETRY_MS = 2000;
  static uint32_t last = 0;
  static bool first = true;
  if (havePdos() || (!first && millis() - last < RETRY_MS)) return;
  first = false;
  last = millis();
  Pdo fresh[NUM_PDOS + 1];
  readPdos(fresh);
  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    if (fresh[idx].type != PdoType::None) {
      Serial.println("Charger profiles found");
      onNewProfiles(fresh);
      return;
    }
  }
}

// The AP33772S sets NEWPDO in its status register when a source sends its
// profiles. If they differ from ours, a new charger was plugged in.
void checkCharger() {
  static uint32_t last = 0, readAt = 0;
  if (millis() - last >= CHARGER_CHECK_MS) {
    last = millis();
    Wire.beginTransmission(AP33772S_ADDRESS);
    Wire.write(CMD_STATUS); // Reading clears it
    Wire.endTransmission();
    if (Wire.requestFrom((uint8_t)AP33772S_ADDRESS, (uint8_t)1) == 1 && (Wire.read() & NEWPDO_MSK))
      readAt = millis() + 300; // Let the negotiation finish first
  }
  if (!readAt || (int32_t)(millis() - readAt) < 0) return;
  readAt = 0;

  Pdo fresh[NUM_PDOS + 1];
  readPdos(fresh);
  bool changed = false;
  for (int idx = 1; idx <= NUM_PDOS; idx++) changed |= fresh[idx] != pdos[idx];
  if (!changed) return;
  stopOff("Charger changed");
  Serial.println("New charger profiles");
  onNewProfiles(fresh);
}

// ---------------------------------------------------------------------------
// Hardware init

void initRotoPD() {
  usbpd.clearConfig(AP33772_CONFIG::UVP_EN); // Required on the Pro: stops UVP from issuing a hard reset
  usbpd.setOutput(0);                        // Output off at boot

  if (!ina238.begin()) {
    Serial.println("INA238 not found");
  }
  ina238.setADCRange(1);
  ina238.setMaxCurrentShunt(7, 0.005); // 5 mOhm shunt, per RotoPD Pro schematic
  ina238.setShuntVoltageConversionTime(INA238_150_us);
  ina238.setAverage(INA238_16_SAMPLES);
  applyHardwareLimits();
  ina238.setDiagnoseAlertBit(INA238_DIAG_ALERT_LATCH);

  retryPdos(); // First read of the charger's profiles; loop() keeps retrying if needed
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
    configTime(0, 0, "pool.ntp.org"); // Clock for log timestamps
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

// Numeric query argument, or -1 when absent
long argOr(const char *name, long fallback = -1) {
  return server.hasArg(name) ? server.arg(name).toInt() : fallback;
}

void handleStatus() {
  char buf[768];
  snprintf(buf, sizeof buf,
           "{\"v\":%.3f,\"i\":%.3f,\"p\":%.3f,\"t\":%.1f,\"on\":%s,\"ramping\":%s,\"trip\":\"%s\","
           "\"note\":\"%s\",\"mode\":\"%s\",\"set_mv\":%d,\"set_ma\":%d,\"ocp_ma\":%d,\"ovp_mv\":%d,"
           "\"opp_mw\":%u,\"ramp_ms\":%d,\"off_s\":%u,\"off_mah\":%u,\"on_s\":%u,\"mah\":%.2f,"
           "\"wh\":%.4f,\"end_ma\":%u,\"end_s\":%u,\"low_s\":%u,\"cable_mohm\":%d,\"cable_a\":%.2f,"
           "\"up_s\":%u,\"log_n\":%d,\"log_s\":%d,\"gen\":%u,\"rssi\":%d}",
           st.v, st.i, st.p, ina238.getTemperature(), st.outputOn ? "true" : "false",
           ramp.active || ramp.pendingOn ? "true" : "false", tripName(), st.note, modeName(),
           cfg.setMv, cfg.setMa, cfg.ocpMa, cfg.ovpMv, cfg.oppMw, cfg.rampMs, cfg.offAfterS,
           cfg.offAfterMah, st.onMs / 1000, st.mah, st.mwh / 1000, cfg.endMa, cfg.endS, st.lowMs / 1000,
           cable.r < 0 ? -1 : (int)(cable.r * 1000 + 0.5f), cable.atA, (unsigned)(millis() / 1000), logCount, cfg.logIntervalS,
           profileGen, WiFi.RSSI());
  server.send(200, "application/json", buf);
}

// Charger profiles
void handleProfiles() {
  String json = "{\"pdos\":[";
  bool first = true;
  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    const Pdo &p = pdos[idx];
    if (p.type == PdoType::None) continue;
    const char *type = p.type == PdoType::Fixed ? "fixed" : p.type == PdoType::PPS ? "pps" : "avs";
    char buf[96];
    snprintf(buf, sizeof buf, "%s{\"idx\":%d,\"type\":\"%s\",\"mv\":%u,\"min_mv\":%u,\"max_mv\":%u,\"ma\":%u}",
             first ? "" : ",", idx, type, p.maxMv, p.minMv, p.maxMv, p.maxMa);
    json += buf;
    first = false;
  }
  json += "],\"floor_mv\":" + String(MIN_SUPPLY_MV) + "}";
  server.send(200, "application/json", json);
}

void handleSet() {
  String mode = server.arg("mode");
  int mv = argOr("mv", 0), ma = argOr("ma", 0);
  if (mode == "pps") reply(requestAdjustable(Mode::PPS, mv, ma));
  else if (mode == "avs") reply(requestAdjustable(Mode::AVS, mv, ma));
  else if (mode == "fixed") reply(requestFixed(argOr("idx", 0), ma));
  else if (mode == "auto") reply(requestVoltage(mv, ma));
  else reply("mode must be auto, pps, avs or fixed");
}

// CSV download of the log. Streams in chunks and keeps the protection loop running meanwhile.
void handleLogCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"labpsu-log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("uptime_s,unix_time,voltage_V,current_A,power_W\n");

  time_t nowUnix = time(nullptr);
  bool clockOk = nowUnix > 1600000000;
  uint32_t nowS = millis() / 1000, newestS = logLastMs / 1000;
  int count = logCount, head = logHead; // Snapshot: entries may be added while we stream
  long since = argOr("since");
  String chunk;
  chunk.reserve(1100);
  for (int k = 0; k < count; k++) {
    const LogEntry &e = logBuf[(head - count + k + LOG_SIZE) % LOG_SIZE];
    uint32_t t = newestS - (count - 1 - k) * cfg.logIntervalS;
    if ((long)t <= since) continue;
    float v = e.mv / 1000.0f, i = e.ma / 1000.0f;
    char line[72];
    if (clockOk)
      snprintf(line, sizeof line, "%u,%lu,%.3f,%.3f,%.3f\n", t, (unsigned long)(nowUnix - (nowS - t)), v, i, v * i);
    else
      snprintf(line, sizeof line, "%u,,%.3f,%.3f,%.3f\n", t, v, i, v * i);
    chunk += line;
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
      sampleTask();
    }
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");
}

// Every endpoint asks for the password when WEB_PASSWORD is set.
ESP8266WebServer::THandlerFunction guarded(ESP8266WebServer::THandlerFunction h) {
  return [h] {
    if (strlen(WEB_PASSWORD) > 0 && !server.authenticate(WEB_USER, WEB_PASSWORD))
      return server.requestAuthentication();
    h();
  };
}

void initServer() {
  server.on("/", HTTP_GET, guarded([] { server.send_P(200, "text/html", WEB_PAGE); }));
  server.on("/api/status", HTTP_GET, guarded(handleStatus));
  server.on("/api/pdos", HTTP_GET, guarded(handleProfiles));
  server.on("/api/log.csv", HTTP_GET, guarded(handleLogCsv));
  server.on("/api/set", HTTP_POST, guarded(handleSet));
  server.on("/api/output", HTTP_POST, guarded([] { reply(setOutput(server.arg("on") == "1")); }));
  server.on("/api/protect", HTTP_POST, guarded([] {
    const char *err = setProtection(argOr("ocp_ma"), argOr("ovp_mv"), argOr("opp_mw"));
    if (!err && server.hasArg("ramp_ms")) err = setRamp(argOr("ramp_ms"));
    reply(err);
  }));
  server.on("/api/clear", HTTP_POST, guarded([] { clearTrip(); reply(nullptr); }));
  server.on("/api/timer", HTTP_POST, guarded([] {
    reply(setTimer(argOr("off_s"), argOr("off_mah"), argOr("end_ma"), argOr("end_s")));
  }));
  server.on("/api/counters/reset", HTTP_POST, guarded([] { resetCounters(); reply(nullptr); }));
  server.on("/api/log", HTTP_POST, guarded([] { reply(setLogInterval(argOr("interval_s"))); }));
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
  Serial.printf("Bus %.3f V  |  %.3f A  |  %.3f W  |  %.1f C  |  output %s%s\n", st.v, st.i, st.p,
                ina238.getTemperature(), st.outputOn ? "ON" : "OFF", st.trip != Trip::None ? " (cutoff tripped)" : "");
  Serial.printf("Set: %s %d mV, %d mA  |  cutoffs: %d mA, %d mV, %u mW (0 = off)  |  soft start %d ms\n",
                modeName(), cfg.setMv, cfg.setMa, cfg.ocpMa, cfg.ovpMv, cfg.oppMw, cfg.rampMs);
  Serial.printf("Counters: %u s on, %.1f mAh, %.3f Wh  |  off after %u s / %u mAh (0 = no limit)\n",
                st.onMs / 1000, st.mah, st.mwh / 1000, cfg.offAfterS, cfg.offAfterMah);
  if (st.note[0]) Serial.printf("Last event: %s\n", st.note);
}

// Charger and cable report
void printPd() {
  if (!havePdos()) {
    Serial.printf("No USB PD profiles (legacy charger, or still negotiating). Input %.2f V\n", st.v);
    return;
  }
  uint32_t maxMw = 0;
  bool fiveAmp = false, epr = false;
  for (int idx = 1; idx <= NUM_PDOS; idx++) {
    const Pdo &p = pdos[idx];
    if (p.type == PdoType::None) continue;
    const char *type = p.type == PdoType::Fixed ? "Fixed" : p.type == PdoType::PPS ? "PPS" : "AVS";
    uint32_t mw = (uint32_t)p.maxMv * p.maxMa / 1000;
    if (p.type == PdoType::Fixed)
      Serial.printf("  %2d  %-5s %s %6.2f V          %.2f A  %5.1f W\n", idx, type, idx >= 8 ? "EPR" : "   ",
                    p.maxMv / 1000.0, p.maxMa / 1000.0, mw / 1000.0);
    else
      Serial.printf("  %2d  %-5s %s %6.2f-%5.2f V  %.2f A  %5.1f W\n", idx, type, idx >= 8 ? "EPR" : "   ",
                    p.minMv / 1000.0, p.maxMv / 1000.0, p.maxMa / 1000.0, mw / 1000.0);
    maxMw = max(maxMw, mw);
    fiveAmp |= p.maxMa >= 5000;
    epr |= idx >= 8;
  }
  Serial.printf("Charger: %.0f W max, PPS %s, AVS %s, EPR %s\n", maxMw / 1000.0,
                findPdo(PdoType::PPS) > 0 ? "yes" : "no", findPdo(PdoType::AVS) > 0 ? "yes" : "no", epr ? "yes" : "no");
  Serial.printf("Cable: %s", epr ? "EPR rated (50 V, 5 A)" : fiveAmp ? "5 A rated (e-marked)" : "3 A, or the charger is limited to 3 A");
  if (cable.r >= 0) Serial.printf(", about %.0f mOhm (measured at %.2f A)\n", cable.r * 1000, cable.atA);
  else Serial.println(", resistance not measured yet (needs > 0.3 A of load)");
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
      "  stat                 measurements, settings and counters\n"
      "  on | off             output switch\n"
      "  set <mV> [mA]        any voltage: picks PPS, fixed or AVS\n"
      "  fix <idx> [mA]       request fixed PDO <idx>\n"
      "  pps <mV> [mA]        request a PPS voltage (needs a PPS charger)\n"
      "  avs <mV> [mA]        request an AVS voltage (needs an EPR charger + 5A cable)\n"
      "  ocp <mA>             over-current cutoff\n"
      "  ovp <mV>             over-voltage cutoff (0 = off)\n"
      "  opp <mW>             over-power cutoff (0 = off)\n"
      "  clear                reset a tripped cutoff\n"
      "  ramp <ms>            soft start duration for PPS/AVS (0 = off)\n"
      "  timer <s>            switch off after this much on-time (0 = off)\n"
      "  mah <mAh>            switch off after this much charge (0 = off)\n"
      "  endcharge <mA> [s]   switch off below <mA> for [s] seconds (0 = off)\n"
      "  pd                   charger and cable report\n"
      "  zero                 reset the on-time / mAh / Wh counters\n"
      "  log <s>              log interval (clears the log)"));
}

void printResult(const char *err, const char *ok) { Serial.println(err ? err : ok); }

void handleCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;

  char cmd[16] = {0};
  long a = -1, b = 0;
  int n = sscanf(line.c_str(), "%15s %ld %ld", cmd, &a, &b);
  String c(cmd);
  c.toLowerCase();

  if (c == "help") printHelp();
  else if (c == "wifi") printWifi();
  else if (c == "scan") i2cScan();
  else if (c == "profile") usbpd.displayProfiles();
  else if (c == "stat") printStatus();
  else if (c == "on") printResult(setOutput(true), "Output ON");
  else if (c == "off") printResult(setOutput(false), "Output OFF");
  else if (c == "set" && n >= 2) printResult(requestVoltage(a, b), "Requested");
  else if (c == "fix" && n >= 2) printResult(requestFixed(a, b), "Fixed PDO requested");
  else if (c == "pps" && n >= 2) printResult(requestAdjustable(Mode::PPS, a, b), "PPS requested");
  else if (c == "avs" && n >= 2) printResult(requestAdjustable(Mode::AVS, a, b), "AVS requested");
  else if (c == "ocp" && n == 2) printResult(setProtection(a, -1, -1), "Over-current cutoff set");
  else if (c == "ovp" && n == 2) printResult(setProtection(-1, a, -1), "Over-voltage cutoff set");
  else if (c == "opp" && n == 2) printResult(setProtection(-1, -1, a), "Over-power cutoff set");
  else if (c == "clear") { clearTrip(); Serial.println("Cutoff reset"); }
  else if (c == "ramp" && n == 2) printResult(setRamp(a), "Soft start set");
  else if (c == "timer" && n == 2) printResult(setTimer(a, -1), "Timer set");
  else if (c == "mah" && n == 2) printResult(setTimer(-1, a), "Charge limit set");
  else if (c == "endcharge" && n >= 2) printResult(setTimer(-1, -1, a, n == 3 ? b : -1), "End of charge set");
  else if (c == "pd") printPd();
  else if (c == "zero") { resetCounters(); Serial.println("Counters reset"); }
  else if (c == "log" && n == 2) printResult(setLogInterval(a), "Log interval set, log cleared");
  else Serial.println("Unknown or incomplete command, type 'help'");
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100); // Short read timeout so serial input never stalls the protection loop
  delay(1000); // Let the PD controller finish negotiating

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setClockStretchLimit(1500); // ESP8266 software I2C needs this for the AP33772S

  Serial.println("\nRotoPD Pro lab supply");
  loadConfig();
  i2cScan();
  initRotoPD();
  initWifi();
  initOta();
  initServer();
  printHelp();
}

void loop() {
  sampleTask();
  rampTask();
  logTask();
  retryPdos();
  checkCharger();
  saveConfigIfDirty();
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }
}
