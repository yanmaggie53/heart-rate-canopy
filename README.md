# Heart Rate Canopy

A canopy with cushions inside. Fairy lights in the canopy change color with the sitter's heart rate; the invitation is to slow it down.

## Try the prototype

1. Install the Pulsoid app on iPhone and Apple Watch, open it on the watch, and start measuring.
2. Get a Pulsoid access token (see `docs/pulsoid-token.md`).
3. Open `web/index.html` in a browser by double-clicking it.
4. Paste the token, press Connect, sit still for 30 seconds while the baseline sets.

No install, no build step. Under Tuning there is a test mode that feeds a fake reading, for checking the colors without a watch.

## Layout

- `web/` the browser prototype
- `firmware/` ESP32 sketch for the gallery piece (not started)
- `tools/` data logging scripts (not started)
- `data/` logged sessions as CSV
- `CLAUDE.md` project context and decisions, read this first
