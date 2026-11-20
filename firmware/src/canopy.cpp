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
// While no monitor is connected, or no reading has arrived for STALE_MS, the string stays dim and ember-warm,
// still breathing the cue so the empty canopy invites a slow breath, and the next reading starts a new sitter.
//
// Serial monitor (115200 baud, port labeled USB): prints what the Bluetooth side is doing and each
// reading as "hr 72  avg 71.4  start 75  level 0.64". Typing a number such as 80 and Enter feeds that bpm
// as a fake reading once a second instead of the Bluetooth one, like the web page's test mode, so the
// lights can be checked without a sensor; typing "live" goes back to the Bluetooth reading. "new" starts a
// new sitter (forgets the start). A palette's name (hearth, coral, candle) switches the colors.
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

// ---------- heart rate monitor ----------
const char*   TARGET_NAME = "COOSPO";  // only connect to a device whose name starts with this, so the canopy never
                                       // grabs a phone running HeartCast or a visitor's own sensor.
                                       // "" = first device with the Heart Rate Service; "Polar" = only Polars, etc.
constexpr unsigned long STALE_MS = 20000;  // no reading for this long -> back to the waiting look

// ---------- look ----------
constexpr int   FRAME_MS      = 33;    // ~30 frames a second
constexpr float WAIT_LEVEL    = 0.25;  // the waiting look's brightness (the calmest sitter gets DIM_FLOOR, in mapping.h)

static const NimBLEUUID HR_SERVICE((uint16_t)0x180D);
static const NimBLEUUID HR_MEASUREMENT((uint16_t)0x2A37);

CRGB    leds[NUM_LEDS];
int8_t  paletteOffset[NUM_LEDS];  // how far ahead of or behind the others each light sits on the gradient
int     paletteIndex = CANOPY_PALETTE;
uint8_t twinklePhase[NUM_LEDS];   // per-light offset so they don't all twinkle together
uint8_t twinkleSpeed[NUM_LEDS];

Smoother      smoother;
StartRate     startRate;                  // where this sitter started (first 30 s)
float         lastHr = 0;                 // most recent raw reading
unsigned long lastReadingMs = 0;          // when it arrived (0 = none yet)
uint8_t       powerScale = 255;           // < 255 while SUPPLY_MA is holding the whole breath back (see drawFrame)
float         shownLevel = WAIT_LEVEL;    // brightness currently on the string, eased toward the target;
                                          // its place between DIM_FLOOR and 1 also sets the warmth
int           testBpm = 0;                // >0 = fake reading from the serial monitor instead of Bluetooth
unsigned long lastTestMs = 0;

// Bluetooth state, shared between the NimBLE callbacks and loop(). Connecting happens in loop(),
// never inside a callback.
enum class BleState { Scanning, FoundDevice, Connecting, Connected };
volatile BleState bleState = BleState::Scanning;
NimBLEAddress foundAddress;               // a copy, not a pointer into the scan results, which a scan restart clears
NimBLEClient* client = nullptr;
char deviceName[32] = "";
volatile int  devicesSeen = 0;            // every advertisement the scan has heard, heart rate or not
constexpr unsigned long SCAN_REPORT_MS = 20000;  // while scanning, say so this often, and start the scan afresh
                                                 // (a device whose name was missed once is otherwise never
                                                 // reported again)
volatile bool firstReading = true;        // first notification since connecting: say what the sensor can tell us
volatile int  contactState = -1;          // -1 unknown/not reported, 0 off the skin, 1 on the skin
QueueHandle_t hrQueue;                    // readings from the Bluetooth task (onNotify) on their way to loop()

// ----- one reading in -----
void newSitter() { smoother.reset(); startRate.reset(); }

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
}

// Heart Rate Measurement: flags byte, then the rate as one byte (or two, little-endian, if flags bit 0 is set),
// then optional energy and RR intervals (beat-to-beat timing) we don't use yet.
// Flags bit 2 = the sensor can tell whether it touches skin, bit 1 = it does. Bit 4 = RR intervals included.
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
  xQueueSend(hrQueue, &hr, 0);             // handled in loop(), so only one task ever touches the session
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
    client->setConnectionParams(12, 12, 0, 150);   // 15 ms interval, 1.5 s supervision timeout
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
  bleState = BleState::Connected;
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

// ----- serial commands: test mode, new sitter, palette -----
void readSerialCommands() {
  static char line[16];
  static int n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n == 0) { /* empty line */ }
      else if (strcmp(line, "live") == 0) {
        testBpm = 0; newSitter(); lastReadingMs = 0;
        Serial.println("test mode off: using the bluetooth reading");
      } else if (strcmp(line, "new") == 0) {
        newSitter();
        Serial.println("new sitter: the next 30 s of readings set where they start");
      } else if (findPalette(line) >= 0) {
        usePalette(findPalette(line));
      } else {
        const int v = atoi(line);
        if (v > 20 && v < 250) {
          if (testBpm == 0) { newSitter(); lastReadingMs = 0; }
          testBpm = v; lastTestMs = 0;
          Serial.printf("test mode: %d bpm once a second (type live to stop)\n", v);
        } else { Serial.print("type a heart rate between 21 and 249, live, new, or a palette:"); printPaletteNames(); }
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
  const bool live = lastReadingMs && now - lastReadingMs < STALE_MS && smoother.hasValue();
  if (lastReadingMs && !live && smoother.hasValue()) {
    Serial.println("no reading for a while: back to the waiting look, the next reading starts a new sitter");
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

  Serial.printf("\ncanopy: %d lights on GPIO %d, brightness %d/255, supply limit %d mA\n", NUM_LEDS, DATA_PIN, BRIGHTNESS, SUPPLY_MA);
  usePalette(CANOPY_PALETTE);
  Serial.printf("dimming and warming: start = first %.0f s, dimmest (%.0f%%) and warmest %.0f bpm below it, "
                "full and palest %.0f above, %.0f s rolling average%s\n",
                START_S, DIM_FLOOR * 100, CALM_DROP, RISE_BPM, SMOOTH_S, STEPPED ? ", stepped" : "");
  Serial.printf("breathing cue: in %.1f s, out %.1f s (%.1f breaths a minute)\n", INHALE_S, EXHALE_S, 60 / (INHALE_S + EXHALE_S));
  Serial.println("type a bpm and Enter for a fake reading, live to go back to bluetooth, new for a new sitter,");
  Serial.print("or a palette name:"); printPaletteNames();

  hrQueue = xQueueCreate(8, sizeof(int));
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  startScan();
}

void loop() {
  static unsigned long lastFrame = 0, lastScanReport = 0;
  if (bleState == BleState::FoundDevice) connectToFound();
  if (bleState == BleState::Scanning && millis() - lastScanReport >= SCAN_REPORT_MS) {
    lastScanReport = millis();
    Serial.printf("still looking for a heart rate monitor%s%s (%d bluetooth signals heard so far). "
                  "is it switched on?\n", TARGET_NAME[0] ? " named " : "", TARGET_NAME, (int)devicesSeen);
    NimBLEDevice::getScan()->start(0, false, true);   // forget what was heard and listen afresh
  }
  int hr;
  while (xQueueReceive(hrQueue, &hr, 0) == pdTRUE) {
    if (testBpm > 0) Serial.printf("(bluetooth hr %d ignored: test mode, type live to use it)\n", hr);
    else onHeartRate(hr, deviceName);
  }
  readSerialCommands();
  const unsigned long now = millis();
  if (now - lastFrame >= FRAME_MS) { lastFrame = now; drawFrame(now); }
  delay(1);
}
