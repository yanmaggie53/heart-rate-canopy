# Heart Rate Canopy

A canopy with cushions inside. Fairy lights in the canopy change color with the sitter's heart rate; the invitation is to slow it down.

Live prototype: https://yanmaggie53.github.io/heart-rate-canopy/

## Try the prototype

1. Install the Pulsoid app on iPhone and Apple Watch, open it on the watch, and start measuring.
2. Get a Pulsoid access token (see `docs/pulsoid-token.md`).
3. Open https://yanmaggie53.github.io/heart-rate-canopy/ (or open `web/index.html` by double-clicking it).
4. Paste the token, press Connect, and sit still. The canopy runs from purple at 55 bpm to red at 95 bpm (adjustable under Tuning); a slower heart turns it cooler.

No install, no build step. Under Tuning there is a test mode that feeds a fake reading, for checking the colors without a watch.

## Try the lights (ESP32)

The firmware is a PlatformIO project. Edit it in any editor (Cursor is fine); Arduino IDE isn't needed.

1. Install PlatformIO once, in its own Python environment:
   ```bash
   python3 -m venv ~/.platformio/penv && ~/.platformio/penv/bin/pip install platformio
   ```
   Then add `export PATH="$PATH:$HOME/.platformio/penv/bin"` to the end of your shell startup file (`~/.bash_profile` or `~/.zshrc`) so `pio` works in new terminals. It goes at the end of PATH so that environment's own `python3` doesn't replace your usual one. (On macOS 13, `brew install platformio` compiles everything from source and takes a very long time.)
2. Wire the string (comments at the top of `firmware/src/light_test.cpp`), check `DATA_PIN`, `NUM_LEDS` and `SUPPLY_MA` there, and plug the ESP32 into your laptop by the board's **USB** port, with a cable that carries data. Charge-only cables are common; if `ls /dev/cu.usbmodem*` shows nothing, try another cable.
3. From `firmware/`, upload the light test and watch what it prints:
   ```bash
   pio run -t upload && pio device monitor
   ```
   It counts the lights, checks the color order, then sweeps the canopy palette from purple to red, printing the hue at each bpm. Ctrl+C leaves the monitor.

   If an upload can't connect, or the board prints `waiting for download`, put it in download mode by hand: hold **BOOT**, tap **RST**, let go of BOOT, then upload again. A board fresh from the factory needs this once.

## Layout

- `web/` the browser prototype, published to GitHub Pages by `.github/workflows/pages.yml` on every push to main
- `firmware/` ESP32 firmware (PlatformIO): `src/light_test.cpp` checks the wiring, `include/mapping.h` is the same heart rate to color mapping as the web page
- `tools/` data logging scripts (not started)
- `data/` logged sessions as CSV
- `CLAUDE.md` project context and decisions, read this first
