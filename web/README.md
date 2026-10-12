# Web UI

Vanilla-JS web interface for the ESP32 Audio/Video Entrainment device. Built
with **esbuild**, gzipped, and packed into the device's SPIFFS `storage`
partition. No framework.

## Layout

```
web/
  src/
    index.html          markup + a few inline layout styles (no inline JS)
    css/style.css        the whole design system (see "Theming" below)
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


## Theming

`css/style.css` defines every colour as a custom property on `:root`, and the
palette is the project site's palette — amber and cyan, because that is the
complementary pair the antiphase flicker mode uses. It is **dark only, on
purpose**: this gets used in a dim room next to a device whose whole job is
controlled light.

Three rules, each of which has already been broken once:

1. **No colour literals.** Not in `style.css`, not in `index.html`'s inline
   styles, not in JS. Inline styles resolve `var()` perfectly well, so markup
   written from JavaScript uses `background:var(--bg-2)` like everything else.
   Inline styles that carry a colour bypass the stylesheet completely, which is
   how the Settings panel ended up rendering light-on-light.

2. **`gen/views/lane_canvas.js` is the one exception**, because a `<canvas>`
   cannot read CSS custom properties. It carries a `PAL` object of opaque
   values that mirrors the tokens by hand; retheme it alongside the stylesheet.

3. **Never rename a selector without grepping `src/js` first.** The JS toggles
   classes on these elements (`.active`, `.on`, `.inactive`, `.locked`,
   `.has-error`, …) and the settings renderer writes inline widths that the
   mobile block overrides with `!important`.

The layout is mobile-first-capable rather than mobile-first: one breakpoint at
640 px stacks the live-control fields, swaps the generator's wide grid for
cards, and turns popovers into bottom sheets. To check it without a phone, the
media queries respond to an **iframe's** width — point one at the dev server at
`width="390"` and the real breakpoints apply.


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

## Three ways to put firmware on the device

Since the dual-boot OTA layout (`plans/ota_dual_boot_plan.md`), pick the right
tool for the job:

- **`./flash_all.sh [--port ...] [--yes] [--dry-run]`** — the **one-time wired
  full flash**. Builds the web UI, the main app, and the OTA updater
  (`../esp32ota`), then writes the bootloader, partition table, otadata seed,
  `ota_0` (main app), `ota_1` (updater) and `storage` (web SPIFFS) in a single
  `esptool write_flash`. This is the **only** way to lay down the new partition
  table — OTA cannot change it, so the first deploy of this layout must be wired.
  Offsets are derived from the built partition table at runtime; flash
  mode/freq/size are read from `build/flash_args`. `--dry-run` builds + verifies
  + prints the command without flashing. `cfgfs` is omitted (formatted on first
  mount).
- **`./flash_web.sh [--port ...]`** — fast **web-UI-only** iteration: rebuilds
  the UI and writes just the `storage` partition by label (no app reflash). Use
  this for day-to-day frontend work.
- **OTA (browser Firmware Update)** — routine **main-app** updates after the
  initial wired flash: the main app reboots into the `ota_1` updater, which
  receives the new `ota_0` image over HTTP and reboots back into it. No USB
  cable needed. This is the normal way to ship new app firmware.

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
