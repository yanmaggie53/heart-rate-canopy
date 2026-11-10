// Light test for the heart rate canopy: proves the ESP32 -> fairy lights half of the pipeline,
// with no heart rate involved. It repeats three stages, and the serial monitor says what you should see.
//
//   1. Counting. Lights come on one at a time, dim white with every tenth one red, so you can check
//      that NUM_LEDS matches the string and the signal reaches the far end.
//   2. Color order. The whole string goes red, then green, then blue, 2 s each. If the colors come out
//      in a different order, change COLOR_ORDER below (most fairy strings are GRB or RGB).
//   3. The canopy palette. A slow sweep from purple (55 bpm) to red (95 bpm) and back, using the same
//      mapping as the web page (include/mapping.h). The monitor prints the bpm each color stands for.
//
// Wiring for a 3-wire WS2812B fairy string:
//   string 5V    -> + of the separate 5V supply (not the ESP32's 5V pin)
//   string GND   -> - of the 5V supply AND a GND pin on the ESP32 (the grounds must be joined)
//   string DATA  -> ESP32 GPIO DATA_PIN through a 330 ohm resistor near the string (fit it: it limits
//                   the current into the pin if the string is unpowered or its GND comes loose)
//   1000 uF cap  -> across the string's 5V and GND at its input, striped leg to GND
//   ESP32        -> laptop by a USB cable that carries data, on the port labeled USB
// Power-up order, uploads included: 5 V adapter first, then USB. Power-down: USB first. Data sent
// into an unpowered string tries to run the lights from the data pin.
// If the first light flickers or shows random colors, the ESP32's 3.3 V data signal may be too weak
// for 5 V lights; a 74AHCT125 level shifter powered from the 5 V supply fixes that.

#include <Arduino.h>
#include <FastLED.h>
#include "mapping.h"

// ---------- hardware: change these to match the bench ----------
constexpr int    DATA_PIN     = 4;     // GPIO the string's data wire goes to (pin labeled 4 on the board;
                                       // on a WROOM-2 never use 35, 36 or 37)
constexpr int    NUM_LEDS     = 200;   // lights on the string (the canopy string has 200)
constexpr EOrder COLOR_ORDER  = GRB;   // confirmed on the canopy string 2026-11-10 (red, green, blue in order)
constexpr int    SUPPLY_VOLTS = 5;
constexpr int    SUPPLY_MA    = 1000;  // FastLED's current limit in mA; it dims the whole string to stay
                                       // under it. FastLED estimates ~43 mA per light at full white but
                                       // real lights draw up to ~60 mA, so set this to about two thirds
                                       // of the adapter's rating. 200 lights at full white would be ~12 A.
                                       // 1000 is a safe placeholder until the adapter's rating is known.
constexpr int    BRIGHTNESS   = 80;    // 0..255. Low until the wiring is proven.

CRGB leds[NUM_LEDS];

// Counting takes about 4 s whatever the string length (20 ms per light for 200).
constexpr int COUNT_STEP_MS = (4000 / NUM_LEDS > 10) ? 4000 / NUM_LEDS : 10;

// Full-saturation color for a hue in degrees, on the same wheel as CSS hsl().
CRGB hueToRgb(float deg) {
  float h = fmodf(deg, 360) / 60;   // 0..6, one unit per sixth of the wheel
  if (h < 0) h += 6;
  float x = 1 - fabsf(fmodf(h, 2) - 1);
  float r, g, b;
  switch ((int)h) {
    case 0:  r = 1; g = x; b = 0; break;   // red -> yellow
    case 1:  r = x; g = 1; b = 0; break;   // yellow -> green
    case 2:  r = 0; g = 1; b = x; break;   // green -> cyan
    case 3:  r = 0; g = x; b = 1; break;   // cyan -> blue
    case 4:  r = x; g = 0; b = 1; break;   // blue -> magenta
    default: r = 1; g = 0; b = x; break;   // magenta -> red
  }
  return CRGB(r * 255, g * 255, b * 255);
}

void countLights() {
  Serial.printf("\n1. counting: %d lights should come on one by one, dim white, every tenth one red\n", NUM_LEDS);
  FastLED.clear(true);
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = (i % 10 == 9) ? CRGB(160, 0, 0) : CRGB(90, 90, 90);
    FastLED.show();
    delay(COUNT_STEP_MS);
  }
  Serial.printf("   all %d on. More lights than that past the end stay dark: raise NUM_LEDS.\n", NUM_LEDS);
  delay(2000);
}

void colorOrder() {
  Serial.println("\n2. color order: the whole string should go red, then green, then blue");
  const struct { CRGB color; const char* name; } steps[] = {
    { CRGB::Red, "red" }, { CRGB::Green, "green" }, { CRGB::Blue, "blue" },
  };
  for (const auto& s : steps) {
    Serial.printf("   now %s\n", s.name);
    fill_solid(leds, NUM_LEDS, s.color);
    FastLED.show();
    delay(2000);
  }
  Serial.println("   wrong order? change COLOR_ORDER at the top of light_test.cpp (GRB <-> RGB fixes most strings)");
}

// Sweep the canopy palette between two whole-number heart rates in tenths of a bpm, about 10 s
// each way. Prints the hue at every whole bpm, so the list can be checked against the web page.
constexpr int STEPS_PER_BPM = 10;

void sweep(int fromBpm, int toBpm) {
  const int dir = toBpm > fromBpm ? 1 : -1;
  const int steps = abs(toBpm - fromBpm) * STEPS_PER_BPM;
  for (int i = 0; i <= steps; i++) {
    const float bpm = fromBpm + dir * (float)i / STEPS_PER_BPM;
    const float hue = hueFor(targetT(bpm));
    fill_solid(leds, NUM_LEDS, hueToRgb(hue));
    FastLED.show();
    if (i % STEPS_PER_BPM == 0) Serial.printf("   %3.0f bpm  hue %3.0f\n", bpm, hue);
    delay(25);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  FastLED.addLeds<WS2812B, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(SUPPLY_VOLTS, SUPPLY_MA);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);
  Serial.printf("\nlight test: %d lights on GPIO %d, brightness %d/255, supply limit %d mA\n",
                NUM_LEDS, DATA_PIN, BRIGHTNESS, SUPPLY_MA);
}

void loop() {
  countLights();
  colorOrder();
  Serial.printf("\n3. canopy palette: purple at %.0f bpm up to red at %.0f bpm, then back down\n", FLOOR_BPM, CEIL_BPM);
  sweep((int)FLOOR_BPM, (int)CEIL_BPM);
  sweep((int)CEIL_BPM, (int)FLOOR_BPM);
}
