# Web UI

Vanilla-JS web interface for the ESP32 Audio/Video Entrainment device. Built
with **esbuild**, gzipped, and packed into the device's SPIFFS `storage`
partition. No framework.

## Layout

```
web/
  src/
    index.html          markup only (no inline JS/CSS)
    css/style.css        styles
    js/
      main.js            entry point: boot, wire events, init
      util.js            showMessage + runtime appConfig
      generator.js       talks to the external generator server
      config.js          config editor / parser / playback / report
      livecontrol.js     live-control panel + channel locking
  build.mjs              esbuild bundle+minify + gzip
  package.json
  dist/                  (generated) uncompressed minified output, for inspection
  data/                  (generated) gzipped assets packed into SPIFFS
```

`dist/` and `data/` are generated and git-ignored.

## Build

```bash
cd web
npm install      # once
npm run build    # -> dist/ (minified) and data/*.gz (shipped to SPIFFS)
```

Then build/flash the firmware (it packs `web/data` into the `storage`
partition via `spiffs_create_partition_image` in `main/CMakeLists.txt`):

```bash
cd ..
source ./activate.sh
idf.py build flash       # full flash includes the SPIFFS image
```

The firmware build does **not** run npm — run `npm run build` first whenever
you change anything under `web/src`.

Convenience scripts (in `freeesp32_ave/`):

- `./build_web.sh [flash]` — build web UI + firmware. `./build_web.sh flash`
  does a **full** flash (app + SPIFFS).
- `./flash_web.sh [--port ...]` — build web UI and flash **only** the `storage`
  SPIFFS partition (fast UI iteration; does not reflash the app). Reset the
  device afterward to load the new page.

## How assets are served

- Only the `.gz` files ship. `main/web_server.c`'s `static_file_handler`
  requests the plain path first, falls back to `<path>.gz`, sets
  `Content-Encoding: gzip`, and derives the MIME type from the pre-`.gz`
  extension. So `index.html` references `/app.js` and `/style.css` (plain
  names) and the device transparently serves the gzipped versions.
- Device-specific config (the generator base URL) is **not** baked into the
  assets. The page fetches `GET /api/appconfig` at boot, which the firmware
  fills from `CONFIG_GENERATOR_SERVER_URL`. This keeps the built assets
  device-independent (and gzip-compatible — no serve-time string templating).
