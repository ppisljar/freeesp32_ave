# Plan: Dual-Boot OTA via a Dedicated Minimal Updater Partition

Status: **Draft / not started.** This document is a plan only. No firmware,
web, or C code is changed by writing it. The orchestrator performs the one-time
wired flash and all on-device testing; subagents may build but **never flash**
(project policy, CLAUDE.md "Hardware Operations").

## Goal

Add field-upgradeable firmware to the `freeesp32_ave` device using an
**asymmetric dual-boot** scheme:

- `ota_0` = the full main application (current `freeesp32_ave` firmware,
  ~1.08 MB today).
- `ota_1` = a small, standalone **OTA updater** whose only job is to receive a
  new image over plain HTTP and flash a target partition, then reboot.

This deliberately avoids the classic "two equal full-app slots" design, which
would waste ~1.5 MB of a 4 MB flash on a second copy of the main app. The
updater is already written, ported to ESP-IDF v5.5.2, and size-stripped — it
lives at `esp32ota/` (sibling of `freeesp32_ave`).

After a **one-time wired full flash** that lays down the new partition table +
both app images, all future **app** updates happen over the air:
`main app → (reboot into updater) → updater receives new ota_0 → reboot into new main app`.

The partition table cannot itself be changed over OTA, so the initial wired
flash is required and accepted.

---

## The OTA updater (`esp32ota/`) — what it already does

Ground truth from `esp32ota/main/main.c` and `esp32ota/main/wifi.c`:

- **`POST /update`** streams the request body into a target partition (4 KB
  scratch buffer). Target resolution (`resolve_target()`):
  - `?part=<label>` — explicit partition label (e.g. `?part=storage`).
  - `?type=app|data&subtype=<n>` — explicit type/subtype.
  - **no query string** — `esp_ota_get_next_update_partition(NULL)`, i.e. the
    next app OTA slot. While running from `ota_1`, that resolves to **`ota_0`**
    (the main-app slot). This is the default upload path.
  - App partitions are written via `esp_ota_begin/write/end` + size check +
    `esp_ota_set_boot_partition(target)` + `esp_restart()` (auto-reboots into
    the freshly flashed app). Data partitions are written via
    `esp_partition_erase_range` + `esp_partition_write` (no reboot).
  - Guards: empty body → 400; image larger than the target partition → 413;
    begin/write/end failures → 500.
- **`GET /reboot`** sets the boot partition to the *other* app slot
  (`esp_ota_get_next_update_partition`) and restarts — i.e. boot back into the
  main app without uploading anything.
- **`GET /`** returns a one-line status: `OTA updater ready (running: <label> @ 0x...)`.
- **WiFi** (`wifi.c`): reads STA credentials from **NVS namespace `"ota"`,
  keys `"ssid"` / `"pass"`** (`OTA_NVS_NAMESPACE` / `OTA_NVS_KEY_SSID` /
  `OTA_NVS_KEY_PASS`). On missing creds or after `WIFI_MAX_RETRY` (6) failed
  joins it falls back to **SoftAP `ESP32-AVE-Setup` / `entrain123` @
  192.168.4.1**. So even if the main app never handed off creds, the updater is
  still reachable on its own AP.
- **Size**: app image 630,896 B (~616 KB), achieved via
  `sdkconfig.defaults` strips: `COMPILER_OPTIMIZATION_SIZE`, `NEWLIB_NANO_FORMAT`,
  `LWIP_IPV6=n`, `MBEDTLS_CERTIFICATE_BUNDLE=n`, `ESP_WIFI_ENTERPRISE_SUPPORT=n`,
  `ESP_WIFI_SOFTAP_SAE_SUPPORT=n`, `FREERTOS_UNICORE=y`, `BOOTLOADER_LOG_LEVEL_NONE`,
  `LOG_DEFAULT_LEVEL_WARN`. **The ~616 KB is a floor, not slack** — see Risks.

---

## Image validation (the updater verifies before it commits)

Validation keeps a corrupt/wrong/truncated upload from ever becoming the booted
image — important precisely because the updater is generic and accepts a
browser-supplied target. Layered, cheapest-first:

1. **Size guard (already present):** reject if `content_len > target->size` (413).
2. **App-image structural validation (already present):** for app targets,
   `esp_ota_end()` validates the ESP-IDF image (magic, segment/header sanity) and
   the bootloader re-validates on boot. A malformed app fails here → no
   `set_boot_partition`, stays in the updater.
3. **SHA-256 content verification (NEW — to add):** the browser sends the
   expected hash of the exact bytes; the updater computes SHA-256 over the
   received stream and refuses to commit on mismatch. This is the only integrity
   check available for **data** partitions (SPIFFS images have no self-describing
   header), and it catches truncation/bit-rot/wrong-file for app images too.
   - Transport: `POST /update?...&sha256=<64-hex>` (or header `X-Expected-SHA256`).
   - Cheap to add: `libmbedcrypto` is **already linked** (for WPA), so
     `mbedtls_sha256_*` streaming costs ~no extra flash. Compute incrementally in
     the existing 4 KB receive loop.
   - On mismatch: app target → abort before `esp_ota_end`/`set_boot` (or call
     `esp_ota_abort`); data target → the freshly written partition is already
     suspect, so erase it again (or mark via a status) and return 422. Either way
     do **not** reboot into it.
   - Browser side: compute the hash with `crypto.subtle.digest('SHA-256', buf)`
     before upload — trivial since the page already holds the `.bin`.

**Enforcement policy (default; confirm):** **mandatory for app** targets (reject
with 400 if `sha256` is absent — the booted firmware must be verified) and
**optional for data** targets (verify only if a hash is supplied, so ad-hoc
`curl` of web assets stays convenient). A compile-time flag in the updater can
relax the app requirement for development.

---

## Phase 0 — Updater: add SHA-256 validation (code change in `esp32ota`)  (effort: S)

A small, self-contained hardening of the updater, independent of the device-side
work. Build-only; no device needed.

**Steps**

1. In `esp32ota/main/main.c` `/update` handler: parse `sha256` (query or
   `X-Expected-SHA256` header) into 32 bytes; if app target and absent → 400
   (per the enforcement policy; gate the app-mandatory check behind a
   `#define`/Kconfig so dev builds can relax it).
2. Initialise `mbedtls_sha256_context`, `mbedtls_sha256_update` over each
   received chunk in the existing receive loop, `mbedtls_sha256_finish` at the
   end; constant-time compare to the expected digest.
3. On mismatch: app → `esp_ota_abort(update_handle)`, no `set_boot`, return 422;
   data → re-erase the written range (or refuse to mark good), return 422. Never
   reboot into an unverified image.
4. Keep behavior unchanged when no hash is required/supplied for data targets.
5. Rebuild; confirm size impact is negligible (mbedcrypto already linked) and the
   image still fits `ota_1`.

**Files touched:** `esp32ota/main/main.c` (+ maybe a tiny `Kconfig.projbuild`
toggle).

**Acceptance:** updater builds clean; a unit/manual check (host-side or on a
spare device by the orchestrator) shows a good hash flashes and a bad/absent hash
is rejected without committing. No flashing by subagents.

---

## Partition table — owned solely by `freeesp32_ave` (the updater is independent)

**The updater is NOT coupled to this table.** `esp32ota` resolves every
partition **at runtime from the table flashed on the device** (the one at
0x8000), via `esp_ota_get_next_update_partition` / `esp_partition_find_first` /
`esp_ota_get_running_partition` — none of which use the updater's own build-time
`partitions.csv`. When we flash *only* the updater's app image into `ota_1`
(Phase 2), its compiled-in table is never written; the device keeps
`freeesp32_ave`'s table. ESP-IDF also relocates an OTA app via the flash MMU
based on whichever slot the bootloader selects, so the *same* updater binary
runs correctly from any app slot on any project.

Consequences:
- `esp32ota/partitions.csv` is a **standalone-build placeholder only** (so
  `idf.py build` for the updater succeeds and you can test it in isolation). It
  does **not** need to match the layout below and is **not** reconciled to it.
  The only requirement it places on a *host* project is structural: the device
  must have an `otadata` partition and at least one app slot other than the one
  the updater runs from.
- The table below is the layout `freeesp32_ave` adopts for **its** device. It is
  authoritative for the *device*, not for the updater project.

`freeesp32_ave` device CSV (4 MB flash; partition table at 0x8000, first partition at 0x9000):

```
# Name,    Type, SubType, Offset,    Size,     Flags
nvs,       data, nvs,     0x9000,    0x4000,
otadata,   data, ota,     0xd000,    0x2000,
phy_init,  data, phy,     0xf000,    0x1000,
ota_0,     app,  ota_0,   0x10000,   0x200000,
ota_1,     app,  ota_1,   ,          0xB0000,
storage,   data, spiffs,  ,          0x80000,
cfgfs,     data, spiffs,  ,          0x80000,
```

### Computed end offsets (verification — sums within 4 MB)

| Partition | Type/Sub      | Offset    | Size      | End       | Notes |
|-----------|---------------|-----------|-----------|-----------|-------|
| nvs       | data/nvs      | 0x009000  | 0x004000  | 0x00D000  | settings (`devcfg`) + updater creds (`ota`) |
| otadata   | data/ota      | 0x00D000  | 0x002000  | 0x00F000  | boot-slot selector |
| phy_init  | data/phy      | 0x00F000  | 0x001000  | 0x010000  | |
| ota_0     | app/ota_0     | 0x010000  | 0x200000  | 0x210000  | **main app** (1.08 MB now → ~0.92 MB headroom) |
| ota_1     | app/ota_1     | 0x210000  | 0x0B0000  | 0x2C0000  | **OTA updater** (630,896 B → ~89 KB / ~12% free) |
| storage   | data/spiffs   | 0x2C0000  | 0x080000  | 0x340000  | web UI assets (uses ~48 KB) |
| cfgfs     | data/spiffs   | 0x340000  | 0x080000  | 0x3C0000  | device configs |
| (free)    | —             | 0x3C0000  | 0x040000  | 0x400000  | ~256 KB unused tail |

End of last partition `0x3C0000` < flash size `0x400000` ✓. Free tail = 256 KB.

> Note vs. the current `freeesp32_ave/partitions.csv`: `nvs` shrinks
> `0x6000 → 0x4000`, `factory` is replaced by `ota_0`+`ota_1`, `otadata` is
> added, and `storage` shrinks `0x180000 → 0x80000` (it only ever uses ~48 KB).
> The full reflash wipes NVS anyway, so the `nvs` resize has no migration cost.

---

## Phase 1 — Partition table + main app becomes `ota_0`  (effort: S)

Make the main app an OTA app and lay down the dual-OTA table.

**Steps**

1. Replace `freeesp32_ave/partitions.csv` with the canonical CSV above
   (add `otadata`; replace `factory` with `ota_0`/`ota_1`; resize `nvs`,
   `storage`).
2. **Do NOT touch `esp32ota/partitions.csv`.** The updater is project-independent
   and resolves partitions from the device's flashed table at runtime (see
   "Partition table" section). Its own CSV is a standalone-build placeholder. Add
   a one-line header comment in `esp32ota/partitions.csv` noting it is a
   placeholder and that runtime resolution uses the host device's table.
3. `sdkconfig` / `sdkconfig.defaults` (`freeesp32_ave`): keep
   `CONFIG_PARTITION_TABLE_CUSTOM=y` + `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`
   (already set), keep `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`. No app-rollback config
   change (see decision below). Delete the stale generated `sdkconfig` line for
   the old table size if present and let it regenerate, or run
   `idf.py set-target esp32` / `reconfigure`.
4. **App-rollback decision — keep OFF to start.** Do **not** set
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. With rollback disabled, a flashed
   `ota_0` is booted unconditionally and the app does **not** need to call
   `esp_ota_mark_app_valid_cancel_rollback()`. Tradeoff: if a bad `ota_0` image
   crash-loops, the bootloader will *not* auto-revert — but recovery is already
   covered because `ota_1` (the updater) is an independent, always-present slot
   reachable via its own SoftAP, and wired reflash is always available. Revisit
   enabling rollback later (would require the main app to mark itself valid in
   `app_main`, plus an updater that respects the pending-verify state).
5. **No mark-valid code needed in the main app for now** — note this explicitly
   so nobody adds a half-finished rollback hook.

**Files touched**

- `freeesp32_ave/partitions.csv` (rewrite — device layout)
- `esp32ota/partitions.csv` (header comment only; NOT reconciled — placeholder)
- `freeesp32_ave/sdkconfig.defaults` (comment only; values already correct)

**Acceptance**

- `freeesp32_ave` builds clean (`./build_web.sh` or `idf.py build`); generated
  `build/partition_table/partition-table.bin` reflects the new layout.
- `esp32ota` builds clean; its app image still fits in `ota_1` (0xB0000) with
  margin (~89 KB free).
- `idf.py size` shows the main app comfortably under `ota_0` (0x200000).
- **Firmware impact:** none functional yet; the change is layout-only, but it
  **invalidates the existing on-device layout → requires a one-time wired full
  flash** (Phase 2) before the device will boot again.

---

## Phase 2 — One-time wired full-flash tooling  (effort: M)

Build both images and assemble the complete first flash. This is **wired-only**
and produced/run by the orchestrator.

**Steps**

1. Add `freeesp32_ave/flash_all.sh` (new script). It:
   - builds the web UI (`web/ npm run build`) — reuse the `build_web.sh` block;
   - builds the main app (`idf.py build` in `freeesp32_ave`);
   - builds the updater (`idf.py build` in `../esp32ota`, after sourcing the
     same `activate.sh`);
   - flashes everything in one `esptool.py write_flash` call at fixed offsets
     (below), OR uses `idf.py flash` for the main-app artifacts plus an explicit
     `esptool.py write_partition`/`write_flash` for the `ota_1` image.
2. Keep `flash_web.sh` unchanged — it already targets `storage` by label via
   `parttool.py`, so it keeps working for fast UI-only iteration after the table
   change (the label still resolves).
3. Document that `cfgfs` does not need an image on first flash (it is formatted
   on first mount); optionally erase its range to guarantee a clean SPIFFS.

**Concrete one-time flash (ESP32, 4 MB, port `$PORT`):**

```bash
# Built artifacts:
#   freeesp32_ave/build/bootloader/bootloader.bin
#   freeesp32_ave/build/partition_table/partition-table.bin
#   freeesp32_ave/build/ota_data_initial.bin      (otadata seed → boot ota_0)
#   freeesp32_ave/build/esp32_audioplayer.bin     (main app → ota_0)
#   freeesp32_ave/build/storage.bin               (web SPIFFS → storage)
#   esp32ota/build/esp32ota.bin                   (updater → ota_1)

esptool.py --chip esp32 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000    freeesp32_ave/build/bootloader/bootloader.bin \
  0x8000    freeesp32_ave/build/partition_table/partition-table.bin \
  0xd000    freeesp32_ave/build/ota_data_initial.bin \
  0x10000   freeesp32_ave/build/esp32_audioplayer.bin \
  0x210000  esp32ota/build/esp32ota.bin \
  0x2c0000  freeesp32_ave/build/storage.bin
```

Notes / options:
- `ota_data_initial.bin` makes the bootloader deterministically select `ota_0`.
  Equivalently, an **erased** otadata region also boots the lowest OTA app
  (`ota_0`); flashing the seed is the explicit, recommended choice.
- The updater binary name follows `esp32ota/`'s `project()` — confirm the
  actual `build/*.bin` filename before wiring the script.
- `--flash_mode/freq/size` should match `freeesp32_ave`'s sdkconfig; copy them
  from `build/flash_args` rather than hard-coding if they differ.
- `cfgfs` (0x340000) intentionally omitted — formatted on first mount.

**Files touched**

- `freeesp32_ave/flash_all.sh` (new)
- (optional) `freeesp32_ave/web/README.md` — add a line describing `flash_all.sh`

**Acceptance**

- Script builds both projects with no errors (subagent may verify the *build*
  half).
- **Orchestrator-only:** wired full flash succeeds; device boots into the main
  app; `flash_web.sh` still updates the UI by label.

---

## Phase 3 — Main-app OTA trigger: `POST /api/ota`  (effort: M)

Add an endpoint to the main app that hands off WiFi creds and reboots into the
updater. Mirror the existing `/api/reboot` handler's deferred-restart pattern in
`web_server.c` (`reboot_timer_cb` + one-shot `esp_timer`, so the HTTP response
flushes first).

**Steps**

1. In `freeesp32_ave/main/web_server.c`, add `static esp_err_t ota_handler(httpd_req_t *req);`
   and register it next to the other exact `/api/*` routes in `web_server_init()`
   (before the wildcard `/*`), e.g.:
   ```c
   httpd_uri_t ota_uri = { .uri = "/api/ota", .method = HTTP_POST,
                           .handler = ota_handler, .user_ctx = NULL };
   httpd_register_uri_handler(g_server_state.server, &ota_uri);
   ```
2. `ota_handler` body:
   - Read current creds from settings: `const device_settings_t *s = settings_get();`
     → `s->wifi_ssid` / `s->wifi_password` (already in `settings.h`).
   - If `s->wifi_ssid[0] == '\0'` (no STA creds — device is on SoftAP only),
     still allow the handoff but respond with a note that the updater will come
     up on its own SoftAP `ESP32-AVE-Setup` @ 192.168.4.1. (Writing an empty
     SSID makes the updater skip STA and go straight to SoftAP, which is the
     correct behavior.)
   - Write creds into NVS for the updater: `nvs_open("ota", NVS_READWRITE, &h)`,
     `nvs_set_str(h, "ssid", s->wifi_ssid)`, `nvs_set_str(h, "pass", s->wifi_password)`,
     `nvs_commit(h)`, `nvs_close(h)`. (Same `nvs` data partition, namespace
     `"ota"` — matches `esp32ota/main/wifi.c`.)
   - Set the boot slot to the updater: `esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL))`
     (running = `ota_0`, so next = `ota_1`). Check the return; on failure send a
     500 JSON error and do **not** reboot.
   - Respond `{"ok":true,"target":"ota_1","ssid_handoff":<bool>}` then schedule
     the deferred `esp_restart()` exactly like `reboot_handler` (reuse the
     `reboot_timer` mechanism or a local one-shot timer).
3. Add includes if not already present: `esp_ota_ops.h`, `nvs.h` (web_server.c
   already includes `esp_system.h`, `settings.h`, `cJSON.h`).
4. Error handling matrix:
   - No creds → still 200, `ssid_handoff:false`, updater uses SoftAP.
   - `nvs_open`/`set`/`commit` fails → 500, no reboot (stay in main app).
   - `esp_ota_set_boot_partition` fails (e.g. updater slot empty/corrupt) → 500,
     no reboot.

**Files touched**

- `freeesp32_ave/main/web_server.c` (new handler + registration + includes)

**Acceptance**

- Builds clean.
- **Orchestrator-only (HTTP-observable, no serial):** `POST /api/ota` returns
  the JSON ack; device reboots; the updater answers `GET /` within a few
  seconds at the same IP (or at 192.168.4.1 if it fell back to SoftAP).

---

## Phase 4 — Browser OTA flow (web UI)  (effort: L)

Add a "Firmware Update" UI to the vanilla-JS frontend. Reuse existing
conventions: tabs in `web/src/js/nav.js` (`TABS` array + `.tab`/`.page`
elements), the `showMessage` helper from `util.js`, and the settings-page
fetch/POST style in `settings.js`.

**Steps**

1. Add a "firmware" entry — either a new tab in `nav.js`'s `TABS` (+ a
   `<section class="page" id="page-firmware">` and a `.tab` button in
   `web/src/index.html`) or a card within the existing Settings page. New tab is
   cleaner given the multi-step flow; pick one and keep `css/style.css`
   consistent.
2. New module `web/src/js/firmware.js` implementing the state machine:
   1. **Pick** a `.bin` (`<input type="file">`); choose target: "Main app
      (ota_0)" [default], "Web assets (storage)", "Configs (cfgfs)".
   2. **Trigger**: `POST /api/ota` → expect `{ok, ssid_handoff}`. Show "Device
      rebooting into updater…".
   3. **Wait for updater**: poll `GET /` (the updater's status line; the main
      app's `/` returns HTML, the updater returns the `OTA updater ready` text —
      distinguish on body content). Handle the IP possibly changing:
      - if `ssid_handoff:true`, poll the *same* host first;
      - on repeated failure, prompt the user that the updater may be on SoftAP
        `ESP32-AVE-Setup` @ `http://192.168.4.1` and let them point the page
        there (cross-origin: the simplest UX is to instruct the user to join the
        AP and open `http://192.168.4.1`, which serves the updater's own minimal
        responses; the rich UI is only in the main app).
   4. **Upload**: first compute the file's SHA-256 in-browser
      (`crypto.subtle.digest('SHA-256', fileBuf)` → hex). Then `POST
      /update?sha256=<hex>` (no other query → `ota_0`; or
      `?part=storage&sha256=...` / `?part=cfgfs&sha256=...` for data targets)
      with the file body. Always send the hash (the updater requires it for the
      app slot; harmless for data). Use `XMLHttpRequest` for `upload.onprogress`
      to drive a progress bar (fetch lacks upload progress). Surface a `422`
      (hash mismatch) distinctly from `413`/`400`/`500`.
   5. For an **app** upload, the updater auto-reboots into the new `ota_0` on
      success — there is no body to wait for. For a **data** upload (storage/
      cfgfs) the updater responds "Data partition flashed." and stays in the
      updater; then call `GET /reboot` to return to the main app.
   6. **Wait for main app**: poll `GET /api/state` (or `/api/appconfig`) until
      the main app answers, then show success and reload.
3. UX for the disconnect/reconnect windows:
   - Clear status banner per phase ("rebooting", "updater online", "uploading
     NN%", "flashing", "booting new firmware", "done").
   - Timeouts with retry buttons; never leave the UI spinning forever.
   - Explicit copy for the SoftAP fallback path (what SSID/IP to use).
   - Failure messaging: upload error (413/400/500 from `/update`), updater
     unreachable → "use SoftAP recovery", main app not returning → "the new
     image may be bad; re-upload via the updater or reflash by USB".
4. Optional: surface the updater's size guard — if the selected `.bin` exceeds
   the target partition the updater returns 413; show that verbatim.

**Files touched**

- `web/src/index.html` (tab button + page section)
- `web/src/js/nav.js` (add `firmware` to `TABS`)
- `web/src/js/firmware.js` (new)
- `web/src/js/main.js` (init the firmware module)
- `web/src/css/style.css` (progress bar / status styles)

**Acceptance**

- `npm run build` succeeds; `flash_web.sh` ships the new UI.
- **Orchestrator-only:** end-to-end happy path works from the browser (Phase 5).

---

## Phase 5 — End-to-end + recovery testing (orchestrator-run)  (effort: M)

All on-device steps are **orchestrator-only**. Per project policy the
orchestrator does **not** read the serial monitor here — rely on UI/HTTP
observability (`GET /`, `/api/state`, the updater's responses, the LED/behavior).

**Test matrix**

1. **Happy path (app):** UI → `POST /api/ota` → updater online → upload new
   `ota_0` → device reboots into new main app → `/api/state` answers.
2. **Happy path (data):** upload a rebuilt `storage.bin` via `?part=storage`,
   then `GET /reboot`; confirm the new UI loads.
3. **Interrupted upload:** kill the upload mid-stream → updater write fails or
   times out; confirm `ota_0` is *not* booted (updater only `set_boot_partition`
   after `esp_ota_end` succeeds) → re-run upload succeeds.
4. **Wrong / corrupt image:** upload a truncated or non-app file → updater
   `esp_ota_end` fails → 500, stays in updater → retry with a good image.
5. **Oversize image:** upload a `.bin` larger than `ota_0` → expect 413.
5b. **Hash mismatch / missing hash:** upload a good image with a wrong
    `?sha256=` → expect 422, no reboot; upload an app image with no `sha256` →
    expect 400 (app-mandatory). Then upload with the correct hash → succeeds.
6. **WiFi-join-fail → SoftAP recovery:** hand off bad creds (or none) → updater
   falls back to `ESP32-AVE-Setup` @ 192.168.4.1; complete the update over the
   AP.
7. **Rollback story:** flash a deliberately bad `ota_0` (e.g. boots and
   immediately reboots). Confirm that with rollback OFF the device does *not*
   auto-revert, and that recovery via the updater (its SoftAP) or wired reflash
   works. Document the observed behavior.

**Acceptance:** every row above passes or has a documented, acceptable outcome.

---

## Flash map (one-time wired flash)

| Offset    | Image / partition                         | What to flash where |
|-----------|-------------------------------------------|---------------------|
| 0x001000  | `build/bootloader/bootloader.bin`         | 2nd-stage bootloader |
| 0x008000  | `build/partition_table/partition-table.bin` | the canonical table |
| 0x00D000  | `build/ota_data_initial.bin`              | otadata seed → boot `ota_0` |
| 0x010000  | `build/esp32_audioplayer.bin` (main app)  | → `ota_0` |
| 0x210000  | `esp32ota/build/esp32ota.bin` (updater)   | → `ota_1` |
| 0x2C0000  | `build/storage.bin` (web SPIFFS)          | → `storage` |
| 0x340000  | (none — formatted on first mount)         | `cfgfs` |

Fast iteration after the one-time flash:
- **Web UI only:** `./flash_web.sh` (label-based `parttool.py write_partition storage`).
- **Main app over the air:** browser Firmware Update → updater → `ota_0`.
- **Updater itself over the air:** browser upload to `?part=ota_1` (rarely
  needed; the updater changes seldom). The updater can flash its *own other*
  slot but not itself while running — to replace `ota_1` you must be running
  `ota_0` and target `?part=ota_1`, then it stays in `ota_0`.

---

## Recovery

If an OTA goes wrong, in order of least to most invasive:

1. **Updater is the safety net.** It lives in `ota_1`, independent of `ota_0`.
   If the new main app is bad, get back into the updater:
   - From a working main app: `POST /api/ota` (boots updater).
   - If the main app won't boot/respond: the bootloader still booted whatever
     `otadata` last selected. Because rollback is OFF, a crash-looping `ota_0`
     won't auto-revert — so the practical recovery is **wired** (step 3) unless
     you can still reach the device long enough to hit `/api/ota`.
2. **SoftAP fallback.** Whenever the updater can't join WiFi (bad/empty creds,
   router down), it serves `ESP32-AVE-Setup` / `entrain123` @
   **http://192.168.4.1**. Join that AP and `POST /update` a known-good
   `ota_0` image; the updater reboots into it.
3. **Wired re-flash.** `./flash_all.sh` (or the `esptool.py write_flash` block
   above) re-lays the full layout over USB. Always works; required if both the
   table and the updater are damaged.

> Because the partition table can't change over OTA, the table + updater are
> effectively "ROM" after the first wired flash. Keep `ota_1` and the table
> stable; treat `ota_0` as the only routinely-OTA'd app.

---

## Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Partition-table change needs a full wired flash; device is bricked-until-flashed after Phase 1 | Phase 2 produces `flash_all.sh`; orchestrator does the one-time wired flash. Communicate that the first deploy is wired. |
| Assuming the updater is tied to a specific partition table | It is NOT — the updater resolves partitions at runtime from the device's flashed table (`esp_ota_*`/`esp_partition_*`) and is relocated by the MMU into whatever app slot it boots from. Its build-time `partitions.csv` is a standalone placeholder. The only host requirement is an `otadata` + a second app slot. Keep it project-independent; do not hardcode offsets. |
| Creds handoff mismatch (namespace/keys) | Main app writes NVS namespace `"ota"`, keys `"ssid"`/`"pass"` — verified against `esp32ota/main/wifi.c`. Empty SSID is valid → updater uses SoftAP. |
| Brownout during flash corrupts `ota_0` | Updater only `set_boot_partition` after `esp_ota_end` succeeds, so a partial write never becomes bootable; `ota_1` + SoftAP remain for recovery. Advise stable USB/PSU during the wired flash. |
| Someone "optimizes" the ~616 KB updater and breaks it | Document that 630,896 B is the *result* of the size-strip in `esp32ota/sdkconfig.defaults`; `ota_1` is sized 0xB0000 (~89 KB / 12% headroom) on purpose. Don't shrink `ota_1` below the real image; don't re-enable IPv6/TLS/enterprise in the updater. |
| Rollback OFF → bad `ota_0` won't auto-revert | Accepted: the updater slot is the recovery path. Documented in Phase 1; revisit enabling `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (+ mark-valid) later if desired. |
| `nvs` shrank 0x6000 → 0x4000 | 16 KB is ample for `devcfg` + `ota` namespaces; the full reflash wipes NVS anyway, so no migration. |
| Browser can't get upload progress with `fetch` | Use `XMLHttpRequest` for `/update` to drive the progress bar. |
| Cross-origin / IP change between main app and SoftAP updater | UI instructs the user to open `http://192.168.4.1` directly when the updater falls back to SoftAP (the updater serves its own minimal endpoints there). |
| `storage` shrank 1.5 MB → 512 KB | Current web assets ~48 KB; 512 KB is comfortable. Monitor if the UI grows. |

---

## Open items / verification

- Confirm the updater's built binary filename in `esp32ota/build/` (depends on
  its `project()` name) before wiring `flash_all.sh`.
- Confirm `freeesp32_ave`'s main app artifact name is `esp32_audioplayer.bin`
  (from `project(esp32_audioplayer)`).
- Confirm flash mode/freq/size from `freeesp32_ave/build/flash_args` and reuse
  them in the wired-flash command rather than hard-coding `dio/40m`.
- Confirm validation enforcement policy: **mandatory SHA-256 for app**, optional
  for data (Phase 0 default) — or relax/strengthen.
- Decide: new "Firmware" tab vs. a card inside Settings (Phase 4).
- Verify the updater's `/` body text so the UI can reliably distinguish updater
  vs. main app when polling.
- Decide whether to also expose updating `ota_1` (the updater) from the UI, or
  keep that an advanced/manual `?part=ota_1` operation.
- Confirm `idf.py build` emits `ota_data_initial.bin` for this table (it does
  when there are ota partitions); otherwise generate it or rely on erased
  otadata.
- Future: revisit anti-rollback once the happy path is proven.

---

## TODO checklist

- [ ] **P0** Add SHA-256 validation to `esp32ota/main/main.c` `/update` (mandatory app / optional data); rebuild, confirm size still fits `ota_1`.
- [ ] **P1** Rewrite `freeesp32_ave/partitions.csv` to the device table.
- [ ] **P1** `esp32ota/partitions.csv`: header comment only (placeholder; NOT reconciled — updater is project-independent).
- [ ] **P1** Confirm `freeesp32_ave` builds; `idf.py size` fits `ota_0`.
- [ ] **P1** Confirm `esp32ota` builds; image fits `ota_1` (0xB0000).
- [ ] **P1** Confirm decision: rollback OFF, no mark-valid code.
- [ ] **P2** Add `freeesp32_ave/flash_all.sh` (build both + assemble full flash).
- [ ] **P2** Verify `flash_web.sh` still works post-table-change (label-based).
- [ ] **P2** (orchestrator) One-time wired full flash; device boots main app.
- [ ] **P3** Add `POST /api/ota` handler + registration in `web_server.c`.
- [ ] **P3** Implement creds handoff to NVS `"ota"` + `set_boot_partition(ota_1)` + deferred restart.
- [ ] **P3** Error handling: no-creds, NVS fail, set-boot fail.
- [ ] **P4** Add Firmware Update UI (`firmware.js`, nav/tab, html, css, main.js).
- [ ] **P4** Compute SHA-256 in-browser; upload via `XHR` with progress + `?sha256=`; handle app vs. data targets; SoftAP messaging; surface 422.
- [ ] **P5** (orchestrator) Run the full test matrix incl. recovery + rollback story.
- [ ] Update `MEMORY.md` / `plans/list.md` when the plan lands and as phases complete.
```
