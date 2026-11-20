// The canopy: reads a heart rate monitor over Bluetooth and dims and warms the fairy lights with it.
//
// Heart rate source: the COOSPO HW807 optical armband (confirmed 2026-11-20, a reading every second).
// It offers the standard Heart Rate Service (0x180D), like a Polar Verity Sense or H10, or an iPhone app
// such as HeartCast relaying the Apple Watch. The ESP32 scans for a device with that service whose name
// starts with TARGET_NAME ("" = the first one heard), connects, subscribes to the Heart Rate Measurement
// characteristic (0x2A37) and gets a reading every time the sensor sends one. If the connection drops,
// or the armband is switched off, it starts scanning again and picks it up when it comes back.
// If the sensor says it has lost skin contact (taken off the arm), its readings are ignored, so the
// lights go back to the waiting look instead of showing a made-up number. (The HW807 doesn't report
// skin contact, so this is for sensors that do.)
//
// Brightness and color: the same mapping as the web page (include/mapping.h). The first 30 s of readings
// set where this sitter started; from there, as the heart slows, the lights fade toward a soft glow and move
// down a warm gradient (include/palette.h) from honey toward ember, reaching the dimmest, deepest end 12 bpm
// below the start; if it speeds up they brighten toward pale warm white. Both follow a 10 s rolling average
// and ease quickly down and slowly up. Each light sits a little apart from the others on the gradient, so a
// few neighboring warm tones show at once. A little slow twinkle per light, stiller as the canopy calms.
// Breathing cue: whatever the heart rate, the whole string swells for INHALE_S (4 s) and fades for EXHALE_S
// (6 s), about six slow breaths a minute with a long out-breath, for the sitter to breathe along with
// (include/palette.h). The web stand-in draws all of this the same way.
// While nobody is there (see "Who is sitting there" below), the string stays dim and ember-warm, still
// breathing the cue so the empty canopy invites a slow breath, and the next reading starts a new sitter.
//
// Relay to the web page: the HW807 takes one Bluetooth connection at a time, so while the board holds it
// the web page can't read it. So the board also advertises a heart rate signal of its own, named
// RELAY_NAME ("Canopy"), and passes on every reading the lights use. The web page connects to it exactly
// as it would to the armband. Between readings, while the lights show the waiting look, it sends
// "nobody there" (skin contact supported but not detected, rate 0) every 2 s, so the page shows its waiting
// look too. Turn it off with RELAY_TO_WEB = false: while on, anyone nearby with a Bluetooth app can read
// the sitter's heart rate, the same as they could read the armband itself.
//
// Who is sitting there: the HW807 can't tell when it's off the arm. Taken off and left switched on, it keeps
// sending made-up readings for about a minute, then goes quiet. So:
//   - Without a seat sensor (SEAT_PIN = -1), a reading counts as missing after a few of the sensor's usual
//     gaps (6 s for a sensor that sends every second, up to 20 s for HeartCast's ~5 s; 20 s while the armband's
//     link is down or has just come back, since a dropped link takes 5 to 7 s to recover), and the lights then
//     go back to the waiting look and the next reading starts a new sitter. The made-up minute still shows.
//   - With a seat sensor (a switch in the cushion that closes to GND while someone sits on it: a pressure mat
//     switch, a button, or a force sensitive resistor), readings only count while someone sits there. Getting
//     up (released for LEAVE_MS) goes straight to the waiting look, and sitting down (pressed for SIT_MS)
//     starts a new sitter, so the made-up minute never shows. Wiring: one wire of the switch to the header
//     pin labeled 5 (next to 4, where the lights' data wire goes), the other to a pin labeled GND; then set
//     SEAT_PIN = 5 below.
//
// Serial monitor (115200 baud, port labeled USB): prints what the Bluetooth side is doing and each
// reading as "hr 72  avg 71.4  start 75  level 0.64". Commands (type and press Enter):
//   80       feed 80 bpm once a second instead of the Bluetooth reading (test mode, shown whatever the seat)
//   live     back to the Bluetooth reading
//   new      start a new sitter (forget where the last one started)
//   hearth   switch the palette on the string (or coral, candle)
//   sit      pretend a seat sensor is fitted and someone sat down (after SIT_MS)
//   leave    pretend they got up (after LEAVE_MS)
//   noseat   stop pretending (back to the real seat sensor, or to none)
//   status   say what is connected and what the lights are doing
//
// Wiring is the same as for the light test (see light_test.cpp and wiring sheet HRC-E-01):
//   string 5V -> separate 5 V supply, string GND -> supply - and an ESP32 GND, string DATA -> GPIO
//   DATA_PIN through 330 ohm, 1000 uF across the string's 5V and GND. 5 V adapter on first, then USB.

#include <Arduino.h>
#include <FastLED.h>
#include <NimBLEDevice.h>
#include "mapping.h"
#include "palette.h"

// ---------- hardware: change these to match the bench ----------
constexpr int    DATA_PIN     = 4;     // GPIO the string's data wire goes to (never 35, 36 or 37 on a WROOM-2)
constexpr int    NUM_LEDS     = 200;   // lights on the string
constexpr EOrder COLOR_ORDER  = BGR;   // checked on the canopy string 2026-11-20: with GRB, red showed green and
                                       // blue showed red (so cyan showed pink). Not GRB as noted on 2026-11-10.
constexpr int    SUPPLY_VOLTS = 5;
constexpr int    SUPPLY_MA    = 1000;  // FastLED's current limit in mA. Set to about two thirds of the adapter's
                                       // rating (FastLED counts ~43 mA per white light, real ones draw ~60).
                                       // 1000 is a safe placeholder until the adapter's rating is known.
constexpr int    BRIGHTNESS   = 80;    // 0..255 overall cap. Low until the wiring is proven.
constexpr int    SEAT_PIN     = -1;    // GPIO of a seat switch in the cushion, wired to GND (5 is free, next to 4).
                                       // -1 = none fitted. Never 35-37, 19-20 (USB), 47-48, or 0, 3, 45, 46.

// ---------- heart rate monitor ----------
const char*   TARGET_NAME = "COOSPO";  // only connect to a device whose name starts with this, so the canopy never
                                       // grabs a phone running HeartCast or a visitor's own sensor.
                                       // "" = first device with the Heart Rate Service; "Polar" = only Polars, etc.
constexpr unsigned long STALE_MIN_MS = 6000;   // a reading counts as missing after STALE_GAPS of the sensor's usual
constexpr unsigned long STALE_MAX_MS = 20000;  // gaps between readings, but never sooner than 6 s (the HW807 sends
constexpr float         STALE_GAPS   = 4;      // every second) or later than 20 s (HeartCast sends every ~5 s)
constexpr unsigned long RECONNECT_GRACE_MS = 3000;  // while the armband's link is down, and this long after it's back,
                                                    // wait the full STALE_MAX_MS: a dropped link (reason 520) takes 5 to 7 s
                                                    // to recover and doesn't mean the sitter left (a removed HW807 stays
                                                    // connected)

// ---------- who is sitting there ----------
constexpr unsigned long SIT_MS   = 1000;   // seat pressed this long = someone sat down
constexpr unsigned long LEAVE_MS = 4000;   // released this long = they got up (shifting weight lifts it briefly)

// ---------- relay to the web page ----------
constexpr bool RELAY_TO_WEB  = true;      // false = the board keeps the heart rate to itself (see the top)
const char*    RELAY_NAME    = "Canopy";  // what the web page's Bluetooth list shows
constexpr unsigned long IDLE_RELAY_MS = 2000;  // while nobody is there, say so this often

// ---------- look ----------
constexpr int   FRAME_MS      = 33;    // ~30 frames a second
constexpr float WAIT_LEVEL    = 0.25;  // the waiting look's brightness (the calmest sitter gets DIM_FLOOR, in mapping.h)

static const NimBLEUUID HR_SERVICE((uint16_t)0x180D);
static const NimBLEUUID HR_MEASUREMENT((uint16_t)0x2A37);
static const NimBLEUUID BATTERY_SERVICE((uint16_t)0x180F);
static const NimBLEUUID BATTERY_LEVEL((uint16_t)0x2A19);

CRGB    leds[NUM_LEDS];
int8_t  paletteOffset[NUM_LEDS];  // how far ahead of or behind the others each light sits on the gradient
int     paletteIndex = CANOPY_PALETTE;
uint8_t twinklePhase[NUM_LEDS];   // per-light offset so they don't all twinkle together
uint8_t twinkleSpeed[NUM_LEDS];

// Everything below is used from loop() only, except where marked: readings arrive on the NimBLE task and are
// handed over through hrQueue, so the smoother and the session are never touched from two tasks at once.
Smoother      smoother;
StartRate     startRate;                  // where this sitter started (first 30 s)
float         lastHr = 0;                 // most recent raw reading
unsigned long lastReadingMs = 0;          // when it arrived (0 = none yet)
uint8_t       powerScale = 255;           // < 255 while SUPPLY_MA is holding the whole breath back (see drawFrame)
float         shownLevel = WAIT_LEVEL;    // brightness currently on the string, eased toward the target;
                                          // its place between DIM_FLOOR and 1 also sets the warmth
int           testBpm = 0;                // >0 = fake reading from the serial monitor instead of Bluetooth
unsigned long lastTestMs = 0;
float         gapMs = 0;                  // the sensor's usual gap between readings (0 = not known yet)
unsigned long prevBtMs = 0;               // when the last Bluetooth reading came in
int           seatSim = -1;               // from "sit"/"leave": -1 not pretending, 0 empty, 1 taken
bool          seatTaken = false;          // the seat, after the SIT_MS / LEAVE_MS holds
bool          seatRawLast = false;
unsigned long seatRawSince = 0;
QueueHandle_t hrQueue = nullptr;          // readings from the NimBLE task (onNotify) to loop()

// Bluetooth state, shared between the NimBLE callbacks and loop(). Connecting happens in loop(),
// never inside a callback.
enum class BleState { Scanning, FoundDevice, Connecting, Connected };
volatile BleState bleState = BleState::Scanning;
NimBLEAddress foundAddress;               // a copy, not a pointer into the scan results, which a scan restart clears
NimBLEClient* client = nullptr;
unsigned long linkUpMs = 0;               // when the armband's link last came up (loop() only)
char deviceName[32] = "";
volatile int  devicesSeen = 0;            // every advertisement the scan has heard, heart rate or not
constexpr unsigned long SCAN_REPORT_MS = 20000;  // while scanning, say so this often, and start the scan afresh
                                                 // (a device whose name was missed once is otherwise never
                                                 // reported again)
volatile bool firstReading = true;        // first notification since connecting: say what the sensor can tell us
volatile int  contactState = -1;          // -1 unknown/not reported, 0 off the skin, 1 on the skin
NimBLEServer*         server = nullptr;   // the relay (RELAY_TO_WEB)
NimBLECharacteristic* relayChr = nullptr;

// ----- who is there -----
bool hasSeat() { return SEAT_PIN >= 0 || seatSim >= 0; }
bool seatPressed() { return seatSim >= 0 ? seatSim == 1 : (SEAT_PIN >= 0 && digitalRead(SEAT_PIN) == LOW); }

// How long without a reading before the lights give up on it: a few of the sensor's usual gaps, or the longest
// wait while the armband's link is down or has only just come back.
unsigned long staleMs() {
  if (testBpm == 0 && (bleState != BleState::Connected || millis() - linkUpMs < RECONNECT_GRACE_MS)) return STALE_MAX_MS;
  if (gapMs <= 0) return STALE_MIN_MS;
  const unsigned long ms = (unsigned long)(STALE_GAPS * gapMs);
  return ms < STALE_MIN_MS ? STALE_MIN_MS : (ms > STALE_MAX_MS ? STALE_MAX_MS : ms);
}

// Someone is there and their heart rate is coming in (test mode counts whatever the seat says).
bool isLive(unsigned long now) {
  const bool seatOk = testBpm > 0 || !hasSeat() || seatTaken;
  return seatOk && lastReadingMs && now - lastReadingMs < staleMs() && smoother.hasValue();
}

// Forget the last sitter: the next reading starts afresh.
void newSitter() { smoother.reset(); startRate.reset(); lastReadingMs = 0; }

void updateSeat(unsigned long now) {
  if (!hasSeat()) return;
  const bool pressed = seatPressed();
  if (pressed != seatRawLast) { seatRawLast = pressed; seatRawSince = now; }
  if (pressed != seatTaken && now - seatRawSince >= (pressed ? SIT_MS : LEAVE_MS)) {
    seatTaken = pressed;
    newSitter();
    Serial.println(seatTaken ? "someone sat down: new sitter, waiting for the armband's reading"
                             : "the cushion is empty: waiting look, armband readings ignored until someone sits");
  }
}

// ----- relay to the web page -----
// Heart Rate Measurement as the web page reads it: flags (bit 2 = can tell contact, bit 1 = someone is there),
// then the rate as one byte.
void relay(int hr, bool present) {
  if (!relayChr) return;
  const uint8_t v[2] = { (uint8_t)(0x04 | (present ? 0x02 : 0)), (uint8_t)(present ? hr : 0) };
  relayChr->notify(v, sizeof v);   // sends these bytes as they are (setValue + notify() could race the NimBLE task)
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { Serial.println("web page connected"); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("web page disconnected (reason %d)\n", reason);   // advertising restarts on its own
  }
} serverCallbacks;

void startRelay() {
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);
  NimBLEService* svc = server->createService(HR_SERVICE);
  relayChr = svc->createCharacteristic(HR_MEASUREMENT, NIMBLE_PROPERTY::NOTIFY);
  server->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(RELAY_NAME);             // in the advertisement itself, so the web page's list shows the name
  adv->addServiceUUID(HR_SERVICE);      // and the service, which is what the web page filters on
  adv->setMinInterval(400);             // every 250 to 500 ms (units of 0.625 ms): plenty for the page to find it,
  adv->setMaxInterval(800);             // and it leaves the radio to the armband's link (the default is every 30-60 ms)
  if (adv->start()) Serial.printf("relaying to the web page as \"%s\"\n", RELAY_NAME);
  else Serial.println("could not start the relay to the web page");
}

// ----- one reading in (loop() only) -----
void onHeartRate(int hr, const char* source) {
  const unsigned long now = millis();
  lastHr = hr;
  lastReadingMs = now;
  const float avg = smoother.push(hr, now);
  const bool  wasFixed = startRate.isFixed();
  startRate.push(hr, now);
  if (startRate.isFixed() && !wasFixed)
    Serial.printf("started at %.0f bpm: dimmest at %.0f, brightest at %.0f\n", startRate.value(),
                  startRate.value() - CALM_DROP, startRate.value() + RISE_BPM);
  const float level = levelFor(levelT(mappedBpm(avg), startRate.value()));
  Serial.printf("hr %3d  avg %5.1f  start %5.1f%s  level %.2f  %s%s\n", hr, avg, startRate.value(),
                startRate.isFixed() ? "" : "?", level, source,
                powerScale < 255 ? "  (held back by SUPPLY_MA)" : "");
  relay(hr, true);
}

// Bluetooth readings handed over by onNotify.
void drainReadings() {
  static unsigned long lastEmptyNote = 0;
  int hr;
  while (hrQueue && xQueueReceive(hrQueue, &hr, 0) == pdTRUE) {
    const unsigned long now = millis();
    if (prevBtMs) {                        // learn the sensor's usual gap, ignoring long silences
      const float gap = now - prevBtMs;
      if (gap <= STALE_MAX_MS) gapMs = gapMs > 0 ? gapMs * 0.8f + gap * 0.2f : gap;
    }
    prevBtMs = now;
    if (testBpm > 0) { Serial.printf("(bluetooth hr %d ignored: test mode, type live to use it)\n", hr); continue; }
    if (hasSeat() && !seatTaken) {         // nobody on the cushion: an armband left on the side isn't a sitter
      if (!lastEmptyNote || now - lastEmptyNote >= 15000) {
        lastEmptyNote = now;
        Serial.printf("(bluetooth hr %d ignored: nobody on the cushion)\n", hr);
      }
      continue;
    }
    onHeartRate(hr, deviceName);
  }
}

// Heart Rate Measurement: flags byte, then the rate as one byte (or two, little-endian, if flags bit 0 is set),
// then optional energy and RR intervals (beat-to-beat timing) we don't use yet.
// Flags bit 2 = the sensor can tell whether it touches skin, bit 1 = it does. Bit 4 = RR intervals included.
// Runs on the NimBLE task: it only filters and hands the reading to loop().
void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  if (len < 2) return;
  const uint8_t flags = data[0];
  int hr;
  if (flags & 0x01) { if (len < 3) return; hr = data[1] | (data[2] << 8); }
  else hr = data[1];
  const bool canTellContact = flags & 0x04;
  const int  contact = canTellContact ? ((flags & 0x02) ? 1 : 0) : -1;
  if (firstReading) {
    firstReading = false;
    Serial.printf("%s: %s, %s\n", deviceName,
                  canTellContact ? "tells whether it touches skin" : "doesn't say whether it touches skin",
                  (flags & 0x10) ? "sends beat timing" : "no beat timing");
  }
  if (contact != contactState) {
    if (contact == 0) Serial.printf("%s is off the skin: ignoring its readings\n", deviceName);
    else if (contact == 1 && contactState == 0) Serial.printf("%s is back on the skin\n", deviceName);
    contactState = contact;
  }
  if (contact == 0) return;                // taken off the arm: whatever number it sends isn't a heart rate
  if (hr <= 20 || hr >= 250) return;       // not a real reading (0 is "sensor not on skin")
  if (hrQueue) xQueueSend(hrQueue, &hr, 0);   // handled in loop(), so only one task ever touches the session
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (bleState != BleState::Scanning) return;
    devicesSeen = devicesSeen + 1;
    if (!dev->isAdvertisingService(HR_SERVICE)) return;
    const std::string name = dev->getName();
    if (TARGET_NAME[0] && name.rfind(TARGET_NAME, 0) != 0) {
      Serial.printf("  skipping %s (%s): not a %s\n", name.empty() ? "(no name)" : name.c_str(),
                    dev->getAddress().toString().c_str(), TARGET_NAME);
      return;
    }
    snprintf(deviceName, sizeof deviceName, "%s", name.empty() ? "heart rate monitor" : name.c_str());
    Serial.printf("found %s (%s), signal %d dBm\n", deviceName, dev->getAddress().toString().c_str(), dev->getRSSI());
    firstReading = true;
    contactState = -1;
    foundAddress = dev->getAddress();
    bleState = BleState::FoundDevice;
    NimBLEDevice::getScan()->stop();
  }
  void onScanEnd(const NimBLEScanResults&, int reason) override {
    if (bleState == BleState::Scanning) {   // scan ran out or was interrupted without finding anything: go again
      Serial.println("still looking for a heart rate monitor...");
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }
} scanCallbacks;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { Serial.printf("connected to %s, waiting for a reading\n", deviceName); }
  void onDisconnect(NimBLEClient*, int reason) override {
    Serial.printf("%s disconnected (reason %d), scanning again\n", deviceName, reason);
    bleState = BleState::Scanning;
    NimBLEDevice::getScan()->start(0, false, true);
  }
} clientCallbacks;

void startScan() {
  Serial.printf("scanning for a heart rate monitor%s%s...\n", TARGET_NAME[0] ? " named " : "", TARGET_NAME);
  bleState = BleState::Scanning;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);             // ask for the name too
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, false, true);           // 0 = until stopped
}

// Called from loop() once the scan has found a device.
void connectToFound() {
  bleState = BleState::Connecting;
  Serial.printf("connecting to %s...\n", deviceName);
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    // 30 to 50 ms between radio exchanges (a reading a second needs no more, and it leaves room for the relay)
    // and a 4 s supervision timeout: at 1.5 s the HW807 dropped now and then, its signal partly blocked by
    // the arm it's on.
    client->setConnectionParams(24, 40, 0, 400);
    client->setConnectTimeout(5000);
  }
  bool ok = client->connect(foundAddress);
  NimBLERemoteService* svc = ok ? client->getService(HR_SERVICE) : nullptr;
  NimBLERemoteCharacteristic* chr = svc ? svc->getCharacteristic(HR_MEASUREMENT) : nullptr;
  if (!chr || !chr->canNotify() || !chr->subscribe(true, onNotify)) {
    Serial.printf("could not %s %s, scanning again\n", ok ? "subscribe to heart rate on" : "connect to", deviceName);
    if (client->isConnected()) client->disconnect();   // onDisconnect restarts the scan
    else startScan();
    return;
  }
  linkUpMs = millis();
  bleState = BleState::Connected;
  // Battery level, if the sensor offers it: worth knowing before it runs out in the middle of a day.
  NimBLERemoteService* bat = client->getService(BATTERY_SERVICE);
  NimBLERemoteCharacteristic* level = bat ? bat->getCharacteristic(BATTERY_LEVEL) : nullptr;
  if (level && level->canRead()) {
    const NimBLEAttValue v = level->readValue();
    if (v.size() >= 1) Serial.printf("%s battery %d%%\n", deviceName, (int)v[0]);   // nothing if the read failed
  }
}

// ----- commands from the serial monitor -----
void printStatus(unsigned long now) {
  Serial.printf("armband: %s\n", bleState == BleState::Connected ? deviceName : "not connected, looking for one");
  Serial.printf("web pages connected: %d\n", server ? (int)server->getConnectedCount() : 0);
  if (seatSim >= 0) Serial.printf("seat: pretend (sit/leave), %s\n", seatTaken ? "someone there" : "empty");
  else if (SEAT_PIN >= 0) Serial.printf("seat: switch on GPIO %d, %s\n", SEAT_PIN, seatTaken ? "someone there" : "empty");
  else Serial.println("seat: none fitted, presence comes from the readings alone");
  Serial.printf("lights: %s; a reading counts as missing after %lu s\n",
                isLive(now) ? (testBpm > 0 ? "test mode" : "live") : "waiting look", staleMs() / 1000);
  if (startRate.hasValue())
    Serial.printf("sitter started at %.0f bpm%s; level %.2f; palette %s%s\n", startRate.value(),
                  startRate.isFixed() ? "" : " (still measuring)", shownLevel, PALETTES[paletteIndex].name,
                  powerScale < 255 ? "; held back by SUPPLY_MA" : "");
  else Serial.printf("no sitter yet; palette %s\n", PALETTES[paletteIndex].name);
}

// ----- palette -----
void usePalette(int index) {
  paletteIndex = index;
  Serial.printf("palette: %s\n", PALETTES[index].name);
}
void printPaletteNames() {
  for (int i = 0; i < NUM_PALETTES; i++) Serial.printf(" %s", PALETTES[i].name);
  Serial.println();
}

void readSerialCommands() {
  static char line[16];
  static int n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n == 0) { /* empty line */ }
      else if (strcmp(line, "live") == 0) {
        testBpm = 0; newSitter();
        Serial.println("test mode off: using the bluetooth reading");
      } else if (strcmp(line, "new") == 0) {
        newSitter();
        Serial.println("new sitter: the next 30 s of readings set where they start");
      } else if (findPalette(line) >= 0) {
        usePalette(findPalette(line));
      } else if (strcmp(line, "sit") == 0 || strcmp(line, "leave") == 0) {
        if (seatSim < 0) { seatTaken = !strcmp(line, "leave"); seatRawLast = seatTaken; seatRawSince = millis(); }
        seatSim = strcmp(line, "sit") == 0 ? 1 : 0;
        Serial.printf("pretending the seat is %s (takes %lu s to count)\n", seatSim ? "taken" : "empty",
                      (seatSim ? SIT_MS : LEAVE_MS) / 1000);
      } else if (strcmp(line, "noseat") == 0) {
        seatSim = -1;
        if (SEAT_PIN >= 0) { seatTaken = seatRawLast = seatPressed(); seatRawSince = millis(); }
        newSitter();
        Serial.println(SEAT_PIN >= 0 ? "stopped pretending: using the seat switch" : "stopped pretending: no seat, presence from the readings alone");
      } else if (strcmp(line, "status") == 0) {
        printStatus(millis());
      } else {
        const int v = atoi(line);
        if (v > 20 && v < 250) {
          if (testBpm == 0) newSitter();
          testBpm = v; lastTestMs = 0;
          Serial.printf("test mode: %d bpm once a second (type live to stop)\n", v);
        } else {
          Serial.print("type a heart rate between 21 and 249, live, new, sit, leave, noseat, status, or a palette:");
          printPaletteNames();
        }
      }
      n = 0;
    } else if (n < (int)sizeof line - 1) line[n++] = c;
  }
  if (testBpm > 0 && millis() - lastTestMs >= 1000) { lastTestMs = millis(); onHeartRate(testBpm, "test"); }
}

// ----- the look -----
void drawFrame(unsigned long now) {
  static unsigned long lastDrawMs = now;
  const float dtS = (now - lastDrawMs) / 1000.0f;
  lastDrawMs = now;
  const bool live = isLive(now);
  // The readings stopped. (When the seat is empty instead, updateSeat() already started a new sitter and said so.)
  if (lastReadingMs && now - lastReadingMs >= staleMs() && smoother.hasValue()) {
    Serial.printf("no reading for %lu s: back to the waiting look, the next reading starts a new sitter\n", staleMs() / 1000);
    newSitter();
  }
  const float sec = now / 1000.0f;
  const float targetLevel = live ? levelFor(levelT(mappedBpm(smoother.value()), startRate.value()))
                                 : WAIT_LEVEL;   // waiting: dim and ember-warm
  shownLevel = easeLevel(shownLevel, targetLevel, dtS);   // quick to dim, slow to brighten

  const float depth = clamp01((shownLevel - DIM_FLOOR) / (1 - DIM_FLOOR));   // 0 at the dimmest, 1 at full: this is
                                                                             // the eased t, so it also sets the warmth
  static float breathS = 0;              // seconds into the breathing cue's cycle, the same for sitter and empty canopy
  breathS = fmodf(breathS + fminf(dtS, 0.25f), INHALE_S + EXHALE_S);   // at most 0.25 s a frame, so after loop() was
                                                                       // blocked (a Bluetooth connect) the cue carries on
                                                                       // from where it stopped instead of jumping
  const float swell = swellFor(breathAt(breathS));                           // in for 4 s, out for 6 s
  const Palette& pal = PALETTES[paletteIndex];
  const float cap = BRIGHTNESS / 255.0f;   // the overall cap, applied here so each light is rounded only once
  static CRGB  color[NUM_LEDS];            // this frame's color and brightness (before the breath) for each light
  static float base[NUM_LEDS];
  for (int i = 0; i < NUM_LEDS; i++) {
    const float wave = sinf(sec * (0.6f + twinkleSpeed[i] / 255.0f * 0.8f) + twinklePhase[i] / 255.0f * 2 * PI);
    base[i]  = twinkleFor(wave, depth) * shownLevel * cap;
    color[i] = ledColor(pal, lightPos(depth, paletteOffset[i] / 127.0f));   // warmer as the heart slows
  }
  auto fill = [&](float breathSwell) {
    for (int i = 0; i < NUM_LEDS; i++) {
      const float a = base[i] * breathSwell;
      leds[i] = CRGB(lroundf(color[i].r * a), lroundf(color[i].g * a), lroundf(color[i].b * a));
    }
  };
  // Power: the supply limit is worked out for the top of the breath and applied to the whole breath, so a tight
  // SUPPLY_MA dims every frame of it evenly instead of FastLED cutting off each inhale, which would flatten the cue.
  fill(swellFor(1));
  powerScale = calculate_max_brightness_for_power_mW(255, (uint32_t)SUPPLY_VOLTS * SUPPLY_MA);
  fill(swell);
  FastLED.show(powerScale);   // this frame is never above the breath's top, so FastLED's own limit doesn't cut in
}

void setup() {
  Serial.begin(115200);
  delay(500);
  FastLED.addLeds<WS2812B, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(SUPPLY_VOLTS, SUPPLY_MA);
  FastLED.setBrightness(255);             // BRIGHTNESS is applied per light in drawFrame, so values are rounded once
  FastLED.setDither(DISABLE_DITHER);      // off anyway below 100 fps; said here so the power estimates in drawFrame
                                          // and in show() count the same thing
  FastLED.clear(true);
  for (int i = 0; i < NUM_LEDS; i++) {
    twinklePhase[i] = random8(); twinkleSpeed[i] = random8();
    paletteOffset[i] = (int8_t)(random8(255) - 127);   // -127..127
  }
  if (SEAT_PIN >= 0) {
    pinMode(SEAT_PIN, INPUT_PULLUP);       // the switch pulls it to GND while someone sits
    seatTaken = seatRawLast = seatPressed();
  }

  Serial.printf("\ncanopy: %d lights on GPIO %d, brightness %d/255, supply limit %d mA\n", NUM_LEDS, DATA_PIN, BRIGHTNESS, SUPPLY_MA);
  usePalette(CANOPY_PALETTE);
  Serial.printf("dimming and warming: start = first %.0f s, dimmest (%.0f%%) and warmest %.0f bpm below it, "
                "full and palest %.0f above, %.0f s rolling average%s\n",
                START_S, DIM_FLOOR * 100, CALM_DROP, RISE_BPM, SMOOTH_S, STEPPED ? ", stepped" : "");
  Serial.printf("breathing cue: in %.1f s, out %.1f s (%.1f breaths a minute)\n", INHALE_S, EXHALE_S, 60 / (INHALE_S + EXHALE_S));
  if (SEAT_PIN >= 0) Serial.printf("seat switch on GPIO %d\n", SEAT_PIN);
  Serial.println("type a bpm for a fake reading, live to go back, new for a new sitter, sit / leave to pretend there's");
  Serial.print("a seat sensor, noseat to stop, status, or a palette name:"); printPaletteNames();

  hrQueue = xQueueCreate(8, sizeof(int));
  NimBLEDevice::init(RELAY_TO_WEB ? RELAY_NAME : "");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  if (RELAY_TO_WEB) startRelay();
  startScan();
}

void loop() {
  static unsigned long lastFrame = 0, lastScanReport = 0, lastIdleRelay = 0;
  if (bleState == BleState::FoundDevice) connectToFound();
  if (bleState == BleState::Scanning && millis() - lastScanReport >= SCAN_REPORT_MS) {
    lastScanReport = millis();
    Serial.printf("still looking for a heart rate monitor%s%s (%d bluetooth signals heard so far). "
                  "is it switched on?\n", TARGET_NAME[0] ? " named " : "", TARGET_NAME, (int)devicesSeen);
    NimBLEDevice::getScan()->start(0, false, true);   // forget what was heard and listen afresh
  }
  drainReadings();
  readSerialCommands();
  const unsigned long now = millis();
  updateSeat(now);
  if (!isLive(now) && now - lastIdleRelay >= IDLE_RELAY_MS) { lastIdleRelay = now; relay(0, false); }
  if (now - lastFrame >= FRAME_MS) { lastFrame = now; drawFrame(now); }
  delay(1);
}
