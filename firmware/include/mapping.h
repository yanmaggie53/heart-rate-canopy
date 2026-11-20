// Heart rate -> brightness and warmth, kept apart from the lights. Must behave exactly like the mapping in
// web/index.html.
//
// Both are measured from where this sitter started rather than on a fixed bpm scale (decided 2026-11-20).
// Classmates said a color that stood for a number made them watch the number and got them nervous; a scale
// that starts from each sitter's own rate has no fixed bpm for any color or brightness. The t below sets how
// bright the lights are and, eased the same way, where they sit on the warm gradient in include/palette.h:
// a slowing heart makes the canopy dimmer and deeper amber, a quickening one brighter and paler.
//
//   start = average of the sitter's readings over their first START_S seconds (the running average until then)
//   t     = (hr - (start - CALM_DROP)) / (CALM_DROP + RISE_BPM), clamped to 0..1
//   level = DIM_FLOOR + (1 - DIM_FLOOR) * t
// So the lights sit at about three quarters when the sitter arrives, fade toward a soft glow (DIM_FLOOR) as
// the heart slows to CALM_DROP below the start, and reach full brightness RISE_BPM above it.
//
// Smoothing: the level follows a rolling average of the readings from the last SMOOTH_S seconds, because raw
// values jitter by 2 to 5 bpm even at rest. On top of that the shown level eases toward its target, quickly
// when dimming and slowly when brightening, so calm is rewarded at once and a cough or a stray thought
// doesn't visibly punish anyone.
// Stepped mode: optionally round the smoothed rate to the nearest STEP_BPM first, so the level changes in
// steps. Kept as an option to compare against the continuous mapping; off by default.

#pragma once
#include <math.h>

constexpr float START_S        = 30;   // the first this-many seconds of readings set where the sitter started
constexpr float CALM_DROP      = 12;   // bpm below the start where the lights reach their dimmest
constexpr float RISE_BPM       = 6;    // bpm above the start where they reach full brightness
constexpr float DIM_FLOOR      = 0.2;  // dimmest level (0..1), so calm is a soft glow, not darkness
constexpr float DIM_TAU_S      = 4;    // time constant in seconds while the lights dim...
constexpr float BRIGHTEN_TAU_S = 12;   // ...and while they brighten
constexpr float SMOOTH_S       = 10;   // rolling-average window in seconds; 0 = use each reading as is
constexpr bool  STEPPED        = false;
constexpr float STEP_BPM       = 2;    // width of one step when STEPPED

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Where a heart rate sits relative to the sitter's start: 0 at CALM_DROP below it, 1 at RISE_BPM above it.
inline float levelT(float hr, float start) {
  return clamp01((hr - (start - CALM_DROP)) / (CALM_DROP + RISE_BPM));
}

// Brightness (0..1) for a position on that scale.
inline float levelFor(float t) {
  return DIM_FLOOR + (1 - DIM_FLOOR) * clamp01(t);
}

// The rate the level is mapped from: the smoothed reading, rounded to a step when STEPPED.
inline float mappedBpm(float smoothedHr) {
  if (!STEPPED) return smoothedHr;
  return (long)(smoothedHr / STEP_BPM + 0.5f) * STEP_BPM;
}

// Move the shown level toward the target over `dtS` seconds: fast down, slow up.
inline float easeLevel(float shown, float target, float dtS) {
  const float tau = target < shown ? DIM_TAU_S : BRIGHTEN_TAU_S;
  return shown + (target - shown) * (1 - expf(-dtS / tau));
}

// Where this sitter started: the average of their raw readings over the first START_S seconds, rounded to a
// whole bpm once that time is up (like "started at" in the web page). Until then, the running average.
class StartRate {
 public:
  void push(float hr, unsigned long nowMs) {
    if (fixed_) return;
    if (count_ == 0) firstMs_ = nowMs;
    sum_ += hr;
    count_++;
    if (nowMs - firstMs_ >= START_S * 1000) { fixed_ = true; sum_ = roundf(sum_ / count_); count_ = 1; }
  }
  bool  hasValue() const { return count_ > 0; }
  bool  isFixed() const { return fixed_; }   // true once the first START_S seconds are in
  float value() const { return count_ ? sum_ / count_ : 0; }
  void  reset() { count_ = 0; sum_ = 0; fixed_ = false; }

 private:
  float sum_ = 0;
  int   count_ = 0;
  bool  fixed_ = false;
  unsigned long firstMs_ = 0;
};

// Rolling average over the readings of the last `windowS` seconds, like pushSample() in the web page.
// Readings come about once a second from an armband or chest strap and every 1 to 5 s from an Apple Watch,
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
