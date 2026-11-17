// Full-saturation color for a hue in degrees, on the same wheel as CSS hsl(), so the string shows the
// same color the web page does. Shared by the light test and the canopy sketch.

#pragma once
#include <FastLED.h>
#include <math.h>

inline CRGB hueToRgb(float deg) {
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
