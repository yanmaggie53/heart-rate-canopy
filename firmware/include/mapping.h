// Heart rate -> hue, kept apart from the lights. Must behave exactly like the mapping in web/index.html.
//
// Fixed scale in bpm, the same for every sitter: purple at the floor, red at the ceiling, continuous in between.
//   t   = (hr - floor) / (ceiling - floor), clamped to 0..1
//   hue = 280 * (1 - t)    degrees on the color wheel: purple, blue, cyan, green, yellow, orange, red
// With the default 55..95 bpm range that is 7 degrees per bpm.
//
// Smoothing: the color follows a rolling average of the readings from the last SMOOTH_S seconds (the web
// page's Smoothing slider, 5 s by default), because raw values jitter by 2 to 5 bpm even at rest.
// Stepped mode: optionally round the smoothed rate to the nearest STEP_BPM before mapping, so the color
// changes in jumps. Kept as an option to compare against the continuous mapping; off by default.

#pragma once

constexpr float FLOOR_BPM = 55;   // purple (web page default, adjustable there under Tuning)
constexpr float CEIL_BPM  = 95;   // red
constexpr float HUE_LOW   = 280;  // hue at the floor
constexpr float HUE_HIGH  = 0;    // hue at the ceiling
constexpr float SMOOTH_S  = 5;    // rolling-average window in seconds; 0 = use each reading as is
constexpr bool  STEPPED   = false;
constexpr float STEP_BPM  = 2;    // width of one color step when STEPPED

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Where a heart rate sits on the scale: 0 at the floor, 1 at the ceiling.
inline float targetT(float hr, float floorBpm = FLOOR_BPM, float ceilBpm = CEIL_BPM) {
  return clamp01((hr - floorBpm) / (ceilBpm - floorBpm));
}

// Hue in degrees for a position on the scale.
inline float hueFor(float t) {
  return HUE_LOW - (HUE_LOW - HUE_HIGH) * clamp01(t);
}

// The rate the color is mapped from: the smoothed reading, rounded to a step when STEPPED.
inline float mappedBpm(float smoothedHr) {
  if (!STEPPED) return smoothedHr;
  return (long)(smoothedHr / STEP_BPM + 0.5f) * STEP_BPM;
}

// Rolling average over the readings of the last `windowS` seconds, like pushSample() in the web page.
// Readings come about once a second from a Polar or chest strap and every 1 to 5 s from an Apple Watch,
// so 32 slots is plenty for a 10 s window.
class Smoother {
 public:
  explicit Smoother(float windowS = SMOOTH_S) : windowMs_(windowS * 1000) {}

  // Add a reading taken at time `nowMs` and return the new smoothed value.
  float push(float hr, unsigned long nowMs) {
    if (windowMs_ <= 0) { smoothed_ = hr; count_ = 0; return smoothed_; }
    if (count_ == CAP) { for (int i = 1; i < CAP; i++) s_[i - 1] = s_[i]; count_--; }
    s_[count_++] = { nowMs, hr };
    // drop what fell out of the window
    int keepFrom = 0;
    while (keepFrom < count_ - 1 && nowMs - s_[keepFrom].ms > windowMs_) keepFrom++;
    if (keepFrom) { for (int i = keepFrom; i < count_; i++) s_[i - keepFrom] = s_[i]; count_ -= keepFrom; }
    float sum = 0;
    for (int i = 0; i < count_; i++) sum += s_[i].hr;
    smoothed_ = sum / count_;
    return smoothed_;
  }
  bool  hasValue() const { return count_ > 0 || (windowMs_ <= 0 && smoothed_ > 0); }
  float value() const { return smoothed_; }
  void  reset() { count_ = 0; smoothed_ = 0; }

 private:
  static constexpr int CAP = 32;
  struct Sample { unsigned long ms; float hr; };
  Sample s_[CAP];
  int    count_ = 0;
  float  smoothed_ = 0;
  unsigned long windowMs_;
};
