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
2. **Later (gallery piece, `firmware/`, started 2026-11-07 with a light test).** ESP32 reads a Polar sensor directly over Bluetooth (standard Heart Rate Service 0x180D, characteristic 0x2A37) and drives WS2812B fairy lights. No laptop in the gallery.

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
- Look of the web stand-in: the field is pure black and only the fairy lights carry color (a soft halo around each, as through fabric). One typeface for everything, Gaegu (Google Fonts), a handwritten face with an uneven baseline, all lowercase, with a faint glow in the current light color. Sizes stay close together: body and controls 15 to 17 px, the reading capped at 90 px (it was 180). Cozy, not sharp. Decided 2026-10-30 after trying Fraunces + Instrument Sans, Fredoka + Nunito and Young Serif; the author wants handwritten, uneven, and no big size jumps.

## Code conventions

- Keep the mapping logic (`targetT`, `hueFor`, smoothing, floor and ceiling) separate from presentation. It is the one piece that moves from the web prototype to the ESP32 unchanged in behavior.
- Plain, dependency-free HTML/JS for the web prototype. No build step.
- Never commit a Pulsoid token. The page keeps it in localStorage only.
- Firmware in Arduino/PlatformIO C++, with the pin numbers, LED count and supply current written at the top of the sketch.
- `firmware/` is one PlatformIO project: each sketch is a `.cpp` in `src/` with its own `[env:...]` in `platformio.ini` (selected by `build_src_filter`), and the mapping lives in `include/mapping.h`, matching the web page. Code is edited in Cursor or here and built and uploaded with the `pio` command line; Arduino IDE isn't used. The board is an ESP32-S3 dev board with an ESP32-S3-WROOM-2 module and two micro-USB ports, USB and UART. esptool reads it (2026-11-07) as ESP32-S3 rev v0.2, 32 MB octal flash and 16 MB PSRAM at 1.8 V, so a WROOM-2 N32R16V. `platformio.ini` sets `board = esp32-s3-devkitc-1` with `board_build.arduino.memory_type = opi_opi` (without it the board won't boot), and GPIO 35 to 37 are off limits. Flash size stays at the 8 MB default, which is plenty.
- The laptop goes on the port labeled **USB** (the S3's own USB, `/dev/cu.usbmodem*`, no driver), and `-DARDUINO_USB_CDC_ON_BOOT=1` sends `Serial` there. On this port DTR/RTS are wired to reset and boot: esptool's usual hard reset after an upload, or a serial tool that drops DTR or RTS, lands the board in download mode ("waiting for download") instead of running the sketch. So `board_upload.after_reset = watchdog-reset`, and anything that reads the port must open it with DTR and RTS left asserted (pyserial's default). If the board is ever stuck in download mode, hold BOOT, tap RST, release BOOT, upload again, or run esptool with `--before no-reset --after watchdog-reset`.
- The board needs a micro-USB cable that carries data. The first one tried only charged: the board lit up but the Mac never saw a USB device. If an upload says "No serial data received", check that a `/dev/cu.usbmodem*` port exists before anything else; without one, PlatformIO used to pick a Bluetooth headphone port (`upload_port` now names the port pattern).
- On the author's Mac (macOS 13), PlatformIO is installed in its own Python environment at `~/.platformio/penv`, not with Homebrew: Homebrew has no prebuilt packages for macOS 13 and compiles everything from source.

## Next tasks

1. Confirm the web prototype connects to Pulsoid and logs a session; commit the CSV under `data/` with a short note on conditions.
2. Run `firmware/src/light_test.cpp` on the real string and set the pin, LED count, color order and supply current it confirms. Done so far: it uploads and runs on the board, and its serial output prints hue 280 at 55 bpm down to 0 at 95, 7 degrees per bpm, the same as the web page (2026-11-07). On 2026-11-10 the string was wired per the sheet below and lights up, and the color-order stage shows red, green, blue in order, so `COLOR_ORDER = GRB` is right. The string has 200 lights (`NUM_LEDS = 200`, set and uploaded 2026-11-10). `SUPPLY_MA` is 1000 as a safe placeholder until the adapter's rating is known; set it to about two thirds of the rating, since FastLED counts ~43 mA per white light against ~60 mA real. 200 lights at full white would be ~12 A, so the limiter always matters, and on thin fairy wire the far end may need power injected (5V and GND only) at higher brightness. Wiring sheet HRC-E-01 (drawing, wire schedule, safety notes; reviewed for electrical, pinout and layout mistakes): https://claude.ai/artifact/Qi5ZjndLqf86Yaz3ZubQvX (private to the author). It also records which header pins to leave alone on this board: 35 to 37 (memory), 19 and 20 (the USB port's data lines), 47 and 48 (1.8 V on this module).
3. Write `firmware/src/canopy.cpp`: FastLED driving one WS2812B string, same mapping and smoothing as the web page, with a serial-print mode for testing without lights. Planned order of heart rate sources: a bpm typed into the serial monitor, then the web page forwarding the Apple Watch reading over USB (Web Serial, Chrome only), then a NimBLE client for the Polar's Heart Rate Service once it arrives. The Apple Watch can't be read by the ESP32 directly because it doesn't broadcast over Bluetooth.
4. Write a small Python logger (`tools/log_hr.py`, using `bleak`) that reads a Polar sensor directly and writes CSV, so data collection doesn't depend on Pulsoid.
5. Decide how a session starts and ends in the gallery with nobody attending (sensor on wrist detected, or a timeout, or a button on the cushion).
