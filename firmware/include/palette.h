// The canopy's colors: a warm gradient that the sitter's calm moves along (decided 2026-11-20).
//
// Position 0 is the calmest end (deep ember), 1 the most stirred (pale warm white). Every stop is warm, so the
// canopy already feels calm when someone sits down; as their heart slows below where they started, the lights
// move toward the ember end while they dim, like a fire burning down. The position comes from the same t that
// sets brightness (levelT in include/mapping.h, eased like the brightness), so color and brightness move
// together, and since t is measured from each sitter's own start, no color stands for a fixed bpm.
// Each light sits a little ahead of or behind the others along the gradient (SPREAD), so the canopy shows a few
// neighboring warm tones at once rather than one flat color.
//
// Must match PALETTES in web/index.html, where they can be compared. The canopy sketch can also switch palettes
// on the string: type a palette's name into the serial monitor.

#pragma once
#include <FastLED.h>
#include <string.h>

struct Palette {
  const char* name;
  int         n;          // how many stops are used
  uint32_t    stops[4];   // 0xRRGGBB from the calmest end to the most stirred, blended evenly
};

constexpr Palette PALETTES[] = {
  { "hearth", 4, { 0xFF3D0D, 0xFF7A1F, 0xFFB347, 0xFFDDB0 } },   // ember, amber, honey, warm white
  { "coral",  4, { 0xE8344A, 0xFF6B5A, 0xFFA07A, 0xFFD6B8 } },   // deep coral, coral, salmon, peach white
  { "candle", 4, { 0xFF8A2A, 0xFFB061, 0xFFD29A, 0xFFE9CF } },   // amber to candle white: a narrower range
};
constexpr int NUM_PALETTES = sizeof PALETTES / sizeof PALETTES[0];

constexpr int   CANOPY_PALETTE = 0;     // index into PALETTES: the one the canopy starts with (0 = hearth)
constexpr float SPREAD         = 0.12;  // how far each light may sit from the others along the gradient (0..0.5)

// Index of the palette with this name, or -1.
inline int findPalette(const char* name) {
  for (int i = 0; i < NUM_PALETTES; i++) if (strcmp(PALETTES[i].name, name) == 0) return i;
  return -1;
}

// Where one light sits on the gradient: t (0 calmest .. 1) squeezed into SPREAD..1-SPREAD, plus that light's
// own offset (-1..1) times SPREAD, so every light stays inside the gradient whatever t is.
inline float lightPos(float t, float offset) {
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  offset = offset < -1 ? -1 : (offset > 1 ? 1 : offset);
  return SPREAD + t * (1 - 2 * SPREAD) + offset * SPREAD;
}

// Color at `pos` (0..1) along a palette, blending between neighboring stops, as a screen color (like the web page).
inline CRGB paletteColor(const Palette& p, float pos) {
  if (p.n == 1) return CRGB(p.stops[0]);
  float x = (pos < 0 ? 0 : pos > 1 ? 1 : pos) * (p.n - 1);
  int i = (int)x;
  if (i >= p.n - 1) i = p.n - 2;
  const CRGB a(p.stops[i]), b(p.stops[i + 1]);
  return blend(a, b, (uint8_t)((x - i) * 255));
}

// The stops are screen colors (sRGB, gamma-encoded), but an LED's PWM duty is linear light: sent as they are, ember
// comes out light orange and warm white nearly white. Decoding them (gamma 2.2) makes the string show the hue the
// web page shows. Only the color is decoded; brightness is applied afterwards, linearly, and the web page matches that
// by drawing each light with alpha^(1/2.2).
inline uint8_t screenToLed(uint8_t v) {
  static uint8_t lut[256];
  static bool ready = false;
  if (!ready) { for (int i = 0; i < 256; i++) lut[i] = (uint8_t)lroundf(255 * powf(i / 255.0f, 2.2f)); ready = true; }
  return lut[v];
}

// Color for an LED at `pos` along a palette, full brightness.
inline CRGB ledColor(const Palette& p, float pos) {
  const CRGB c = paletteColor(p, pos);
  return CRGB(screenToLed(c.r), screenToLed(c.g), screenToLed(c.b));
}

// ---------- the look: breathing cue and twinkle, shared by both sketches and drawn the same way in the web page ----------

// The breathing cue (decided 2026-11-20, replacing breathing at the heart rate): the whole canopy swells for INHALE_S as if
// breathing in and fades for EXHALE_S, about six breaths a minute with a longer out-breath, the pace slow-breathing studies
// use. The sitter can breathe with it without being told a number. It keeps the same pace whether or not anyone is
// sitting, so the empty canopy invites a slow breath too, and it doesn't follow the heart rate, which shows only as
// brightness and warmth.
constexpr float INHALE_S = 4;
constexpr float EXHALE_S = 6;

// How full the breath is (0 empty .. 1 full), `s` seconds into a cycle: a smooth rise over INHALE_S, a smooth fall over
// EXHALE_S, with no jolt where they meet.
inline float breathAt(float s) {
  const float cycle = INHALE_S + EXHALE_S, pi = 3.14159265f;
  s = fmodf(s, cycle);
  if (s < 0) s += cycle;
  return s < INHALE_S ? 0.5f - 0.5f * cosf(pi * s / INHALE_S) : 0.5f + 0.5f * cosf(pi * (s - INHALE_S) / EXHALE_S);
}

// Brightness factor for the breath: 0.5 with the breath out, 0.9 with it in. The same depth at every level, so the cue
// stays easy to follow even at the calm end.
inline float swellFor(float breath) { return 0.5f + 0.4f * breath; }

// A light's slow twinkle: wave (-1..1) is that light's own slow sine; depth is 0 at the dimmest, calmest end and 1 at full
// brightness. The twinkle gets shallower as the canopy calms, so a calm canopy is a still one apart from the breath.
inline float twinkleFor(float wave, float depth) { return 1 - (0.15f + 0.35f * depth) * (0.5f - 0.5f * wave); }
