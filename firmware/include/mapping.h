// Heart rate -> hue, kept apart from the lights. Must behave exactly like the mapping in web/index.html.
//
// Fixed scale in bpm, the same for every sitter: purple at the floor, red at the ceiling, continuous in between.
//   t   = (hr - floor) / (ceiling - floor), clamped to 0..1
//   hue = 280 * (1 - t)    degrees on the color wheel: purple, blue, cyan, green, yellow, orange, red
// With the default 55..95 bpm range that is 7 degrees per bpm.
//
// Smoothing and the stepped mode are added with the canopy sketch.

#pragma once

constexpr float FLOOR_BPM = 55;   // purple (web page default, adjustable there under Tuning)
constexpr float CEIL_BPM  = 95;   // red
constexpr float HUE_LOW   = 280;  // hue at the floor
constexpr float HUE_HIGH  = 0;    // hue at the ceiling

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Where a heart rate sits on the scale: 0 at the floor, 1 at the ceiling.
inline float targetT(float hr, float floorBpm = FLOOR_BPM, float ceilBpm = CEIL_BPM) {
  return clamp01((hr - floorBpm) / (ceilBpm - floorBpm));
}

// Hue in degrees for a position on the scale.
inline float hueFor(float t) {
  return HUE_LOW - (HUE_LOW - HUE_HIGH) * clamp01(t);
}
