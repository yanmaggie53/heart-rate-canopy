// The canopy: reads a heart rate monitor over Bluetooth and colors the fairy lights with it.
//
// Heart rate source: any Bluetooth device offering the standard Heart Rate Service (0x180D), which is
// what a Polar Verity Sense or H10 broadcasts, and also what an iPhone app such as HeartCast or
// HR Broadcast puts out when it relays the Apple Watch's reading. The ESP32 scans for the first such
// device (or one whose name starts with TARGET_NAME if that is set), connects, subscribes to the Heart
// Rate Measurement characteristic (0x2A37) and gets a reading every time the sensor sends one. If the
// connection drops it starts scanning again.
//
// Color: the same fixed scale as the web page (include/mapping.h), purple at 55 bpm through blue, cyan,
// green, yellow and orange to red at 95, following a rolling average of the last 5 s of readings. The
// lights breathe (brightness swells and fades) at the current heart rate, with a little slow twinkle per
// light, the way the web stand-in draws them. The hue eases toward its target rather than jumping.
// While no monitor is connected, or no reading has arrived for STALE_MS, the string breathes slowly and
// dimly in purple so it is not just dark.
//
// Serial monitor (115200 baud, port labeled USB): prints what the Bluetooth side is doing and each
// reading as "hr 72  avg 71.4  hue 165". Typing a number such as 80 and Enter feeds that bpm as a fake
// reading once a second instead of the Bluetooth one, like the web page's test mode, so the lights can
// be checked without a sensor; typing "live" goes back to the Bluetooth reading.
//
// Wiring is the same as for the light test (see light_test.cpp and wiring sheet HRC-E-01):
//   string 5V -> separate 5 V supply, string GND -> supply - and an ESP32 GND, string DATA -> GPIO
//   DATA_PIN through 330 ohm, 1000 uF across the string's 5V and GND. 5 V adapter on first, then USB.

#include <Arduino.h>
#include <FastLED.h>
#include <NimBLEDevice.h>
#include "mapping.h"
#include "hue_rgb.h"

// ---------- hardware: change these to match the bench ----------
constexpr int    DATA_PIN     = 4;     // GPIO the string's data wire goes to (never 35, 36 or 37 on a WROOM-2)
constexpr int    NUM_LEDS     = 200;   // lights on the string
constexpr EOrder COLOR_ORDER  = GRB;   // confirmed on the canopy string 2026-11-10
constexpr int    SUPPLY_VOLTS = 5;
constexpr int    SUPPLY_MA    = 1000;  // FastLED's current limit in mA. Set to about two thirds of the adapter's
                                       // rating (FastLED counts ~43 mA per white light, real ones draw ~60).
                                       // 1000 is a safe placeholder until the adapter's rating is known.
constexpr int    BRIGHTNESS   = 80;    // 0..255 overall cap. Low until the wiring is proven.

// ---------- heart rate monitor ----------
const char*   TARGET_NAME = "";        // "" = first device with the Heart Rate Service; "Polar" = only Polars, etc.
constexpr unsigned long STALE_MS = 20000;  // no reading for this long -> back to the waiting look

// ---------- look ----------
constexpr int   FRAME_MS      = 33;    // ~30 frames a second
constexpr float HUE_EASE      = 0.06;  // share of the remaining hue difference closed each frame (web: 0.03 at 60 fps)
constexpr float WAIT_BPM      = 12;    // the waiting look breathes at this rate, like slow breathing
constexpr float WAIT_LEVEL    = 0.25;  // and this dim

static const NimBLEUUID HR_SERVICE((uint16_t)0x180D);
static const NimBLEUUID HR_MEASUREMENT((uint16_t)0x2A37);

CRGB    leds[NUM_LEDS];
uint8_t twinklePhase[NUM_LEDS];   // per-light offset so they don't all twinkle together
uint8_t twinkleSpeed[NUM_LEDS];

Smoother      smoother;
float         lastHr = 0;                 // most recent raw reading
unsigned long lastReadingMs = 0;          // when it arrived (0 = none yet)
float         shownHue = HUE_LOW;         // hue currently on the string, eased toward the target
float         shownT = 0;                 // position on the scale, eased
int           testBpm = 0;                // >0 = fake reading from the serial monitor instead of Bluetooth
unsigned long lastTestMs = 0;

// Bluetooth state, shared between the NimBLE callbacks and loop(). Connecting happens in loop(),
// never inside a callback.
enum class BleState { Scanning, FoundDevice, Connecting, Connected };
volatile BleState bleState = BleState::Scanning;
const NimBLEAdvertisedDevice* foundDevice = nullptr;
NimBLEClient* client = nullptr;
char deviceName[32] = "";
volatile int  devicesSeen = 0;            // every advertisement the scan has heard, heart rate or not
constexpr unsigned long SCAN_REPORT_MS = 20000;  // while scanning, say so this often

// ----- one reading in -----
void onHeartRate(int hr, const char* source) {
  const unsigned long now = millis();
  lastHr = hr;
  lastReadingMs = now;
  const float avg = smoother.push(hr, now);
  const float hue = hueFor(targetT(mappedBpm(avg)));
  Serial.printf("hr %3d  avg %5.1f  hue %3.0f  %s\n", hr, avg, hue, source);
}

// Heart Rate Measurement: flags byte, then the rate as one byte (or two, little-endian, if flags bit 0 is set),
// then optional energy and RR intervals we don't use yet.
void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  if (len < 2) return;
  const uint8_t flags = data[0];
  int hr;
  if (flags & 0x01) { if (len < 3) return; hr = data[1] | (data[2] << 8); }
  else hr = data[1];
  if (hr <= 20 || hr >= 250) return;       // not a real reading (0 is "sensor not on skin")
  if (testBpm > 0) { Serial.printf("(bluetooth hr %d ignored: test mode, type live to use it)\n", hr); return; }
  onHeartRate(hr, deviceName);
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
    foundDevice = dev;
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
  bool ok = client->connect(foundDevice);
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

// ----- test mode from the serial monitor -----
void readSerialCommands() {
  static char line[16];
  static int n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n == 0) { /* empty line */ }
      else if (strcmp(line, "live") == 0) {
        testBpm = 0; smoother.reset(); lastReadingMs = 0;
        Serial.println("test mode off: using the bluetooth reading");
      } else {
        const int v = atoi(line);
        if (v > 20 && v < 250) {
          if (testBpm == 0) { smoother.reset(); lastReadingMs = 0; }
          testBpm = v; lastTestMs = 0;
          Serial.printf("test mode: %d bpm once a second (type live to stop)\n", v);
        } else Serial.println("type a heart rate between 21 and 249, or live");
      }
      n = 0;
    } else if (n < (int)sizeof line - 1) line[n++] = c;
  }
  if (testBpm > 0 && millis() - lastTestMs >= 1000) { lastTestMs = millis(); onHeartRate(testBpm, "test"); }
}

// ----- the look -----
void drawFrame(unsigned long now) {
  const bool live = lastReadingMs && now - lastReadingMs < STALE_MS && smoother.hasValue();
  if (lastReadingMs && !live && smoother.hasValue()) {
    Serial.println("no reading for a while: back to the waiting look");
    smoother.reset();
  }
  const float sec = now / 1000.0f;
  float targetHue, targetTVal, breathBpm, level;
  if (live) {
    targetTVal = targetT(mappedBpm(smoother.value()));
    targetHue = hueFor(targetTVal);
    breathBpm = smoother.value();
    level = 1;
  } else {                               // waiting: slow, dim, purple
    targetTVal = 0;
    targetHue = HUE_LOW;
    breathBpm = WAIT_BPM;
    level = WAIT_LEVEL;
  }
  // ease the hue the short way round the wheel, and the scale position
  float d = fmodf(targetHue - shownHue + 540, 360) - 180;
  shownHue = fmodf(shownHue + d * HUE_EASE + 360, 360);
  shownT += (targetTVal - shownT) * HUE_EASE;

  const CRGB base = hueToRgb(shownHue);
  const float beat = 0.5f + 0.5f * sinf(sec * (breathBpm / 60) * 2 * PI);   // the lights breathe at the heart rate
  const float swell = 0.45f + 0.45f * beat * (0.4f + 0.6f * shownT);        // a faster heart also swells harder
  for (int i = 0; i < NUM_LEDS; i++) {
    const float tw = 0.75f + 0.25f * sinf(sec * (0.6f + twinkleSpeed[i] / 255.0f * 0.8f) + twinklePhase[i] / 255.0f * 2 * PI);
    const float a = swell * tw * level;
    leds[i] = base;
    leds[i].nscale8_video((uint8_t)(a * 255));
  }
  FastLED.show();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  FastLED.addLeds<WS2812B, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(SUPPLY_VOLTS, SUPPLY_MA);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);
  for (int i = 0; i < NUM_LEDS; i++) { twinklePhase[i] = random8(); twinkleSpeed[i] = random8(); }

  Serial.printf("\ncanopy: %d lights on GPIO %d, brightness %d/255, supply limit %d mA\n", NUM_LEDS, DATA_PIN, BRIGHTNESS, SUPPLY_MA);
  Serial.printf("scale: purple at %.0f bpm to red at %.0f, %.0f s rolling average%s\n", FLOOR_BPM, CEIL_BPM, SMOOTH_S,
                STEPPED ? ", stepped" : "");
  Serial.println("type a bpm and Enter for a fake reading, live to go back to bluetooth");

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  startScan();
}

void loop() {
  static unsigned long lastFrame = 0, lastScanReport = 0;
  if (bleState == BleState::FoundDevice) connectToFound();
  if (bleState == BleState::Scanning && millis() - lastScanReport >= SCAN_REPORT_MS) {
    lastScanReport = millis();
    Serial.printf("still looking for a heart rate monitor (%d bluetooth signals heard so far, none from one)\n", (int)devicesSeen);
  }
  readSerialCommands();
  const unsigned long now = millis();
  if (now - lastFrame >= FRAME_MS) { lastFrame = now; drawFrame(now); }
  delay(1);
}
