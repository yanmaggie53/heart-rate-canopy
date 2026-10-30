# Heart Rate Canopy

Project context for Claude Code. Ideation and planning happen in a separate Claude chat; this repo is where the code lives. Read this file before touching anything.

## What this is

A piece for Maggie's Advanced New Media class, shown in a gallery where visitors use it unattended. A fabric canopy with two or three handmade cushions inside. Addressable fairy lights inside the canopy change color based on the sitter's heart rate. The goal for the sitter is to bring their heart rate down toward a calm target. Theme is unwinding in the middle of a busy life, with physiological data as the way to bring embodied experience into the piece.

Builds on two earlier projects by the same author that turned physiological data into a material output (MEmento, and an EEG-driven embroidery machine).

## Pipeline

```
heart rate sensor --(Bluetooth)--> controller --(data line)--> addressable LED fairy string
```

Two stages:

1. **Now (web prototype, `web/index.html`).** Apple Watch streams heart rate through the Pulsoid app to a websocket; a single HTML page reads it and paints the screen as a stand-in canopy. Hosted at https://yanmaggie53.github.io/heart-rate-canopy/ by GitHub Pages (`.github/workflows/pages.yml` publishes the `web/` folder on every push to main); it also runs as a local file by double-clicking. The Pulsoid websocket is wss and is not restricted by page origin (a handshake probe from a github.io origin was rejected for the token, not the origin), so the hosted page can use it. An earlier note here claimed only a local page could; that was wrong.
2. **Later (gallery piece, `firmware/`, not started).** ESP32 reads a Polar sensor directly over Bluetooth (standard Heart Rate Service 0x180D, characteristic 0x2A37) and drives WS2812B fairy lights. No laptop in the gallery.

## Decisions already made

- Sensor for the gallery is a Polar Verity Sense (optical arm band) because strangers will put it on; Polar H10 chest strap is the alternative for the author's own testing and gives beat timing. Apple Watch is only for prototyping (one reading every ~5 s, no Bluetooth broadcast).
- Lights are WS2812B / NeoPixel addressable fairy strings on thin wire, 3-wire (5V, data, ground). Two-wire strings don't work with standard libraries. Separate 5V supply, not the ESP32's USB.
- Controller is an ESP32 dev board. Use NimBLE-Arduino for Bluetooth (the stock ESP32 BLE library crashes on Polar H10/H9) and FastLED or Adafruit NeoPixel for the lights.
- Heart rate is mapped to hue continuously, not in bins, with a rolling average over the last 5 to 10 s, because raw values jitter by 2 to 5 bpm even at rest and a stepped palette strobes. A stepped mode (new color every N bpm) stays available as an option to compare.
- The scale is fixed in bpm, not relative to the sitter. Visitors are seated, neither sleeping nor exercising, and mostly students, so a realistic seated range reads the same for everyone and every bpm has its own color. Defaults: floor 55 bpm, ceiling 95 bpm, both adjustable under Tuning. Mapping: t = (hr - floor) / (ceiling - floor), clamped 0..1. Decided 2026-10-27, replacing the per-sitter scale (baseline from the first 30 s, calm target = baseline - 20). The first-30-s average is still kept as "started at", but it only feeds the line under the reading.
- Color runs down the wheel from purple at the floor (hue 280) through blue, cyan, green, yellow and orange to red at the ceiling (hue 0), continuously: hue = 280·(1 - t). With the default range that is 7 degrees per bpm, so a change of one bpm is a visible shift. Decided 2026-10-27, replacing the blue-to-magenta scale of 2026-10-26.
- Test mode (under Tuning in the web page) feeds an exact fake reading from a slider, once a second and whenever the slider moves, so each color can be checked without a watch. Connecting to Pulsoid turns it off and starts a fresh session.
- Lights "breathe" (brightness oscillates) at the current heart rate, so a fast heart reads as flicker and a slow one as breathing. With the H10's beat timing this can become a true per-beat pulse.
- Heart rate is physiological data. The gallery needs a sign saying it's measured and whether it's stored. Logged data is anonymous (no names, no timestamps that identify a person).
- Look of the web stand-in: the field is pure black and only the fairy lights carry color (a soft halo around each, as through fabric). Type is rounded and soft, all lowercase: Fredoka for the big number and title, Nunito for everything else, with a faint glow in the current light color. Cozy, not sharp. Decided 2026-10-30, replacing the Fraunces + Instrument Sans pairing and the colored background wash.

## Code conventions

- Keep the mapping logic (`targetT`, `hueFor`, smoothing, floor and ceiling) separate from presentation. It is the one piece that moves from the web prototype to the ESP32 unchanged in behavior.
- Plain, dependency-free HTML/JS for the web prototype. No build step.
- Never commit a Pulsoid token. The page keeps it in localStorage only.
- Firmware in Arduino/PlatformIO C++, with the pin numbers, LED count and supply current written at the top of the sketch.

## Next tasks

1. Confirm the web prototype connects to Pulsoid and logs a session; commit the CSV under `data/` with a short note on conditions.
2. Write `firmware/canopy.ino`: NimBLE client for the Heart Rate Service, FastLED driving one WS2812B string, same mapping as the web page. Include a serial-print mode for testing without lights.
3. Write a small Python logger (`tools/log_hr.py`, using `bleak`) that reads a Polar sensor directly and writes CSV, so data collection doesn't depend on Pulsoid.
4. Decide how a session starts and ends in the gallery with nobody attending (sensor on wrist detected, or a timeout, or a button on the cushion).
