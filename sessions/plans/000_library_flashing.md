# Plan 000 — Session Library: structure + flashing to the device

Status: **Planned.** Wires the `sessions/library/*.ledc` files onto the device so
they ship as built-in sessions. No session content here (see 001+); this is the
build/flash integration only.

## Goal
Pack `sessions/library/*.ledc` into the device's `cfgfs` SPIFFS partition so they
appear in the web UI's config list out of the box, flashed during a normal
`idf.py flash` / `flash_all.sh`.

## Background (current state)
- `main/CMakeLists.txt` already packs the web UI: `spiffs_create_partition_image(storage ../web/data FLASH_IN_PROJECT)`.
- `cfgfs` (partition label `cfgfs`, mounted at `WEB_CFG_BASE` in `web_server.c`) holds user `.ledc` configs. It is currently **not** pre-seeded — formatted empty on first mount.
- The web config list reads from `cfgfs` (configstore). So seeding `cfgfs` makes the library show up with zero firmware-logic changes.

## Approach (recommended): seed `cfgfs` from `sessions/library`
1. In `main/CMakeLists.txt`, add a second image:
   ```cmake
   spiffs_create_partition_image(cfgfs ../sessions/library FLASH_IN_PROJECT)
   ```
   This packs only `sessions/library/` (which contains **only** `.ledc` files — guidelines/plans live one level up and are excluded). Built and flashed by `idf.py flash` and by `flash_all.sh` (add `cfgfs` to its image list at offset `0x340000`).
2. `flash_all.sh` / flash map: add `0x340000  build/cfgfs.bin  -> cfgfs`. (Currently cfgfs is omitted/formatted-on-mount; with the image it is written explicitly.)
3. Verify `cfgfs` partition (0x80000 = 512 KB) easily holds the library: each `.ledc` is a few KB; 9 sessions ≪ 512 KB.

## Over-the-air delivery (no wired flash): `push_library.sh`
The wired seed above is the **factory / first-provision** path. For a unit
already on the network, the library can be delivered OTA **without touching the
partition image**, because `cfgfs` is a file store exposed over HTTP:
`PUT /api/configs/<name>` writes one `.ledc` into `cfgfs` (`store_put` in
`web_server.c`); `GET /api/configs` lists them. `sessions/push_library.sh
<device-ip>` loops the 9 files through `PUT`. This is **additive** — it does
NOT erase user-saved configs (unlike a full flash, which reseeds the whole
cfgfs image). Same-named files overwrite in place. The script runs the local
`validate_session.mjs` gate before pushing (override `--no-validate`; preview
`--dry-run`).

Note: OTA proper (the `esp32ota` updater) targets only the **app** slot
(`ota_0`); it has no concept of a data partition, so `storage`/`cfgfs` are not
part of OTA. The HTTP file-push above is the supported network path for cfgfs.
A future option to make the library ride OTA app updates is to embed it in the
app binary (`EMBED_FILES`) and merge built-in + user lists in the config API
(firmware change; out of scope here).

## Tradeoff to document
- `FLASH_IN_PROJECT` means a **full flash overwrites `cfgfs`** → user-saved configs are replaced by the shipped library. Acceptable for the one-time wired flash / factory image; note it in `flash_all.sh` output. `flash_web.sh` (storage-only) and OTA (ota_0 app only) do **not** touch `cfgfs`, so routine updates preserve user configs.
- If preserving user configs across full flashes becomes important later: ship the library read-only in `storage` instead and merge built-in + user lists in the config API (firmware change). Out of scope for v1.

## Filename / metadata convention
- Library files: `library/<NN>_<slug>.ledc` (e.g. `01_sleep_onset.ledc`) so they sort sensibly in the list. Must match `configstore` `NAME_RE` (`*.ledc`).
- Each file starts with a `#` header comment: name, goal, duration, target bands, and a one-line safety note (firmware ignores `#` lines).

## Validation gate (CI / pre-flash)
- A small Node script runs the generator's `parse`/`serialize`/`validate` (`web/src/js/gen`) over every `library/*.ledc`: must parse clean, round-trip, and raise no validate errors. Treat `config_parser.c` as canonical.
- A safety linter check: no LED flicker `frequency` in 15–25 Hz at high brightness; light ramps in/out ≥10 s; brightness within cap (per `SESSION_DESIGN_GUIDELINES.md` §4).

## Steps
- [x] Add `spiffs_create_partition_image(cfgfs ...)` to `main/CMakeLists.txt`.
- [x] Add `cfgfs.bin` @ `0x340000` to `flash_all.sh` (+ note the overwrite tradeoff). Wired into offset resolution, existence/fit checks, the esptool arg list, and the interactive confirmation warning.
- [x] ~~Add `sessions/library/.gitkeep`~~ — N/A: 9 real `.ledc` files have landed.
- [x] Validation script `sessions/validate_session.mjs` present; all 9 library files validate 0 errors / 0 warnings.
- [x] Build → `build/cfgfs.bin` generated (0x80000, full-partition SPIFFS image); `flash_all.sh --dry-run` verifies fit (cfgfs @ 0x340000, end 0x3c0000, no overlap) and assembles the esptool command including cfgfs.
- [ ] (orchestrator) Full flash → confirm sessions appear in the web config list.

## Acceptance
- `idf.py build` emits `build/cfgfs.bin`; all `library/*.ledc` validate clean + pass the safety lint.
- After a full flash, the built-in sessions are listed and playable in the web UI; `flash_web.sh` and OTA leave them/user configs per the tradeoff above.
